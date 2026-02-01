package models

import (
	"encoding/json"
	"testing"

	"github.com/google/uuid"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// TestTieringConfig_Validation tests TieringConfig validation within ClusterCreateRequest
func TestTieringConfig_Validation(t *testing.T) {
	tests := []struct {
		name        string
		request     *ClusterCreateRequest
		expectError error
		description string
	}{
		{
			name: "tiering disabled - no validation required",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierDev,
				Provider: CloudProviderAWS,
				Region:   "us-east-1",
				TieringConfig: &TieringConfig{
					Enabled: false,
				},
			},
			expectError: nil,
			description: "When tiering is disabled, no duration or S3 config validation",
		},
		{
			name: "tiering enabled - all fields provided",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:         true,
					HotDuration:     "7d",
					WarmDuration:    "30d",
					ColdDuration:    "90d",
					CompressionType: "zstd",
				},
				S3Config: &S3Config{
					Endpoint:        "s3.amazonaws.com",
					Bucket:          "orochi-cold-tier",
					Region:          "us-west-2",
					AccessKeyID:     "AKIAIOSFODNN7EXAMPLE",
					SecretAccessKey: "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
				},
			},
			expectError: nil,
			description: "Valid tiering configuration with all required fields",
		},
		{
			name: "tiering enabled - missing hot duration",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:      true,
					WarmDuration: "30d",
					ColdDuration: "90d",
				},
				S3Config: &S3Config{
					Endpoint:        "s3.amazonaws.com",
					Bucket:          "bucket",
					Region:          "us-west-2",
					AccessKeyID:     "key",
					SecretAccessKey: "secret",
				},
			},
			expectError: ErrTieringHotDurationRequired,
			description: "Hot duration is required when tiering is enabled",
		},
		{
			name: "tiering enabled - missing warm duration",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:      true,
					HotDuration:  "7d",
					ColdDuration: "90d",
				},
				S3Config: &S3Config{
					Endpoint:        "s3.amazonaws.com",
					Bucket:          "bucket",
					Region:          "us-west-2",
					AccessKeyID:     "key",
					SecretAccessKey: "secret",
				},
			},
			expectError: ErrTieringWarmDurationRequired,
			description: "Warm duration is required when tiering is enabled",
		},
		{
			name: "tiering enabled - missing cold duration",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:      true,
					HotDuration:  "7d",
					WarmDuration: "30d",
				},
				S3Config: &S3Config{
					Endpoint:        "s3.amazonaws.com",
					Bucket:          "bucket",
					Region:          "us-west-2",
					AccessKeyID:     "key",
					SecretAccessKey: "secret",
				},
			},
			expectError: ErrTieringColdDurationRequired,
			description: "Cold duration is required when tiering is enabled",
		},
		{
			name: "tiering enabled - invalid compression type",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:         true,
					HotDuration:     "7d",
					WarmDuration:    "30d",
					ColdDuration:    "90d",
					CompressionType: "gzip", // Invalid - only lz4, zstd allowed
				},
				S3Config: &S3Config{
					Endpoint:        "s3.amazonaws.com",
					Bucket:          "bucket",
					Region:          "us-west-2",
					AccessKeyID:     "key",
					SecretAccessKey: "secret",
				},
			},
			expectError: ErrTieringInvalidCompression,
			description: "Only lz4 and zstd compression types are valid",
		},
		{
			name: "tiering enabled - lz4 compression valid",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:         true,
					HotDuration:     "7d",
					WarmDuration:    "30d",
					ColdDuration:    "90d",
					CompressionType: "lz4",
				},
				S3Config: &S3Config{
					Endpoint:        "s3.amazonaws.com",
					Bucket:          "bucket",
					Region:          "us-west-2",
					AccessKeyID:     "key",
					SecretAccessKey: "secret",
				},
			},
			expectError: nil,
			description: "lz4 is a valid compression type",
		},
		{
			name: "tiering enabled - empty compression type (allowed)",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:         true,
					HotDuration:     "7d",
					WarmDuration:    "30d",
					ColdDuration:    "90d",
					CompressionType: "", // Will default to zstd
				},
				S3Config: &S3Config{
					Endpoint:        "s3.amazonaws.com",
					Bucket:          "bucket",
					Region:          "us-west-2",
					AccessKeyID:     "key",
					SecretAccessKey: "secret",
				},
			},
			expectError: nil,
			description: "Empty compression type is allowed (will be defaulted)",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.request.Validate()
			if tt.expectError != nil {
				assert.ErrorIs(t, err, tt.expectError, tt.description)
			} else {
				assert.NoError(t, err, tt.description)
			}
		})
	}
}

// TestS3Config_Validation tests S3Config validation within ClusterCreateRequest
func TestS3Config_Validation(t *testing.T) {
	tests := []struct {
		name        string
		request     *ClusterCreateRequest
		expectError error
		description string
	}{
		{
			name: "tiering enabled - missing S3 config",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:      true,
					HotDuration:  "7d",
					WarmDuration: "30d",
					ColdDuration: "90d",
				},
				S3Config: nil, // Missing S3 config
			},
			expectError: ErrS3ConfigRequired,
			description: "S3 config is required when tiering is enabled",
		},
		{
			name: "tiering enabled - missing S3 endpoint",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:      true,
					HotDuration:  "7d",
					WarmDuration: "30d",
					ColdDuration: "90d",
				},
				S3Config: &S3Config{
					Bucket:          "bucket",
					Region:          "us-west-2",
					AccessKeyID:     "key",
					SecretAccessKey: "secret",
				},
			},
			expectError: ErrS3EndpointRequired,
			description: "S3 endpoint is required",
		},
		{
			name: "tiering enabled - missing S3 bucket",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:      true,
					HotDuration:  "7d",
					WarmDuration: "30d",
					ColdDuration: "90d",
				},
				S3Config: &S3Config{
					Endpoint:        "s3.amazonaws.com",
					Region:          "us-west-2",
					AccessKeyID:     "key",
					SecretAccessKey: "secret",
				},
			},
			expectError: ErrS3BucketRequired,
			description: "S3 bucket is required",
		},
		{
			name: "tiering enabled - missing S3 region",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:      true,
					HotDuration:  "7d",
					WarmDuration: "30d",
					ColdDuration: "90d",
				},
				S3Config: &S3Config{
					Endpoint:        "s3.amazonaws.com",
					Bucket:          "bucket",
					AccessKeyID:     "key",
					SecretAccessKey: "secret",
				},
			},
			expectError: ErrS3RegionRequired,
			description: "S3 region is required",
		},
		{
			name: "tiering enabled - missing S3 access key",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:      true,
					HotDuration:  "7d",
					WarmDuration: "30d",
					ColdDuration: "90d",
				},
				S3Config: &S3Config{
					Endpoint:        "s3.amazonaws.com",
					Bucket:          "bucket",
					Region:          "us-west-2",
					SecretAccessKey: "secret",
				},
			},
			expectError: ErrS3AccessKeyRequired,
			description: "S3 access key is required",
		},
		{
			name: "tiering enabled - missing S3 secret key",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:      true,
					HotDuration:  "7d",
					WarmDuration: "30d",
					ColdDuration: "90d",
				},
				S3Config: &S3Config{
					Endpoint:    "s3.amazonaws.com",
					Bucket:      "bucket",
					Region:      "us-west-2",
					AccessKeyID: "key",
				},
			},
			expectError: ErrS3SecretKeyRequired,
			description: "S3 secret key is required",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := tt.request.Validate()
			assert.ErrorIs(t, err, tt.expectError, tt.description)
		})
	}
}

// TestShardCount_Validation tests shard count validation
func TestShardCount_Validation(t *testing.T) {
	tests := []struct {
		name        string
		shardCount  int
		expectError error
		description string
	}{
		{
			name:        "negative shard count",
			shardCount:  -1,
			expectError: ErrInvalidShardCount,
			description: "Shard count cannot be negative",
		},
		{
			name:        "zero shard count (valid - will be defaulted)",
			shardCount:  0,
			expectError: nil,
			description: "Zero shard count is valid and will be defaulted",
		},
		{
			name:        "valid shard count - 1",
			shardCount:  1,
			expectError: nil,
			description: "1 shard is valid",
		},
		{
			name:        "valid shard count - 16",
			shardCount:  16,
			expectError: nil,
			description: "16 shards is valid",
		},
		{
			name:        "valid shard count - 1024",
			shardCount:  1024,
			expectError: nil,
			description: "1024 shards is the maximum valid value",
		},
		{
			name:        "invalid shard count - 1025",
			shardCount:  1025,
			expectError: ErrShardCountTooHigh,
			description: "Shard count cannot exceed 1024",
		},
		{
			name:        "invalid shard count - 2000",
			shardCount:  2000,
			expectError: ErrShardCountTooHigh,
			description: "Shard count cannot exceed 1024",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req := &ClusterCreateRequest{
				Name:              "test-cluster",
				Tier:              ClusterTierDev,
				Provider:          CloudProviderAWS,
				Region:            "us-east-1",
				DefaultShardCount: tt.shardCount,
			}

			err := req.Validate()
			if tt.expectError != nil {
				assert.ErrorIs(t, err, tt.expectError, tt.description)
			} else {
				assert.NoError(t, err, tt.description)
			}
		})
	}
}

// TestApplyDefaults_Tiering tests that ApplyDefaults correctly sets tiering defaults
func TestApplyDefaults_Tiering(t *testing.T) {
	tests := []struct {
		name                string
		request             *ClusterCreateRequest
		expectedCompression string
		description         string
	}{
		{
			name: "tiering enabled - compression defaults to zstd",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:      true,
					HotDuration:  "7d",
					WarmDuration: "30d",
					ColdDuration: "90d",
					// CompressionType not set
				},
			},
			expectedCompression: "zstd",
			description:         "Default compression should be zstd",
		},
		{
			name: "tiering enabled - compression explicitly set to lz4",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierProduction,
				Provider: CloudProviderAWS,
				Region:   "us-west-2",
				TieringConfig: &TieringConfig{
					Enabled:         true,
					HotDuration:     "7d",
					WarmDuration:    "30d",
					ColdDuration:    "90d",
					CompressionType: "lz4",
				},
			},
			expectedCompression: "lz4",
			description:         "Explicit compression type should be preserved",
		},
		{
			name: "tiering disabled - no compression default",
			request: &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     ClusterTierDev,
				Provider: CloudProviderAWS,
				Region:   "us-east-1",
				TieringConfig: &TieringConfig{
					Enabled: false,
				},
			},
			expectedCompression: "",
			description:         "Compression should not be set when tiering is disabled",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			tt.request.ApplyDefaults()

			if tt.request.TieringConfig != nil && tt.request.TieringConfig.Enabled {
				assert.Equal(t, tt.expectedCompression, tt.request.TieringConfig.CompressionType, tt.description)
			} else if tt.request.TieringConfig != nil {
				assert.Empty(t, tt.request.TieringConfig.CompressionType, tt.description)
			}
		})
	}
}

// TestApplyDefaults_ShardCount tests that ApplyDefaults correctly sets shard count based on tier
func TestApplyDefaults_ShardCount(t *testing.T) {
	tests := []struct {
		name               string
		tier               ClusterTier
		explicitShardCount int
		expectedShardCount int
		description        string
	}{
		{
			name:               "dev tier - default shard count",
			tier:               ClusterTierDev,
			explicitShardCount: 0,
			expectedShardCount: 4,
			description:        "Dev tier should default to 4 shards",
		},
		{
			name:               "starter tier - default shard count",
			tier:               ClusterTierStarter,
			explicitShardCount: 0,
			expectedShardCount: 8,
			description:        "Starter tier should default to 8 shards",
		},
		{
			name:               "production tier - default shard count",
			tier:               ClusterTierProduction,
			explicitShardCount: 0,
			expectedShardCount: 16,
			description:        "Production tier should default to 16 shards",
		},
		{
			name:               "enterprise tier - default shard count",
			tier:               ClusterTierEnterprise,
			explicitShardCount: 0,
			expectedShardCount: 32,
			description:        "Enterprise tier should default to 32 shards",
		},
		{
			name:               "explicit shard count overrides default",
			tier:               ClusterTierDev,
			explicitShardCount: 64,
			expectedShardCount: 64,
			description:        "Explicit shard count should override tier default",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req := &ClusterCreateRequest{
				Name:              "test-cluster",
				Tier:              tt.tier,
				Provider:          CloudProviderAWS,
				Region:            "us-east-1",
				DefaultShardCount: tt.explicitShardCount,
			}

			req.ApplyDefaults()

			assert.Equal(t, tt.expectedShardCount, req.DefaultShardCount, tt.description)
		})
	}
}

// TestApplyDefaults_EnableColumnar tests columnar storage defaults
func TestApplyDefaults_EnableColumnar(t *testing.T) {
	tests := []struct {
		name             string
		tier             ClusterTier
		expectedColumnar bool
		description      string
	}{
		{
			name:             "dev tier - columnar disabled",
			tier:             ClusterTierDev,
			expectedColumnar: false,
			description:      "Dev tier should not enable columnar by default",
		},
		{
			name:             "starter tier - columnar disabled",
			tier:             ClusterTierStarter,
			expectedColumnar: false,
			description:      "Starter tier should not enable columnar by default",
		},
		{
			name:             "production tier - columnar enabled",
			tier:             ClusterTierProduction,
			expectedColumnar: true,
			description:      "Production tier should enable columnar by default",
		},
		{
			name:             "enterprise tier - columnar enabled",
			tier:             ClusterTierEnterprise,
			expectedColumnar: true,
			description:      "Enterprise tier should enable columnar by default",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req := &ClusterCreateRequest{
				Name:     "test-cluster",
				Tier:     tt.tier,
				Provider: CloudProviderAWS,
				Region:   "us-east-1",
			}

			req.ApplyDefaults()

			assert.Equal(t, tt.expectedColumnar, req.EnableColumnar, tt.description)
		})
	}
}

// TestCluster_JSONSerialization tests JSON serialization/deserialization of Cluster with tiering
func TestCluster_JSONSerialization(t *testing.T) {
	hotDuration := "7d"
	warmDuration := "30d"
	coldDuration := "90d"
	compression := "zstd"
	s3Endpoint := "s3.amazonaws.com"
	s3Bucket := "orochi-cold"
	s3Region := "us-west-2"
	orgID := uuid.New()

	cluster := &Cluster{
		ID:                  uuid.New(),
		Name:                "test-cluster",
		OwnerID:             uuid.New(),
		OrganizationID:      &orgID,
		Status:              ClusterStatusRunning,
		Tier:                ClusterTierProduction,
		Provider:            CloudProviderAWS,
		Region:              "us-west-2",
		Version:             "1.0.0",
		NodeCount:           3,
		NodeSize:            "large",
		StorageGB:           200,
		PoolerEnabled:       true,
		TieringEnabled:      true,
		TieringHotDuration:  &hotDuration,
		TieringWarmDuration: &warmDuration,
		TieringColdDuration: &coldDuration,
		TieringCompression:  &compression,
		S3Endpoint:          &s3Endpoint,
		S3Bucket:            &s3Bucket,
		S3Region:            &s3Region,
		EnableColumnar:      true,
		DefaultShardCount:   16,
	}

	// Serialize to JSON
	jsonData, err := json.Marshal(cluster)
	require.NoError(t, err, "Failed to marshal cluster to JSON")

	// Deserialize from JSON
	var deserializedCluster Cluster
	err = json.Unmarshal(jsonData, &deserializedCluster)
	require.NoError(t, err, "Failed to unmarshal cluster from JSON")

	// Verify all tiering fields are preserved
	assert.Equal(t, cluster.TieringEnabled, deserializedCluster.TieringEnabled)
	require.NotNil(t, deserializedCluster.TieringHotDuration)
	assert.Equal(t, *cluster.TieringHotDuration, *deserializedCluster.TieringHotDuration)
	require.NotNil(t, deserializedCluster.TieringWarmDuration)
	assert.Equal(t, *cluster.TieringWarmDuration, *deserializedCluster.TieringWarmDuration)
	require.NotNil(t, deserializedCluster.TieringColdDuration)
	assert.Equal(t, *cluster.TieringColdDuration, *deserializedCluster.TieringColdDuration)
	require.NotNil(t, deserializedCluster.TieringCompression)
	assert.Equal(t, *cluster.TieringCompression, *deserializedCluster.TieringCompression)

	// Verify S3 fields
	require.NotNil(t, deserializedCluster.S3Endpoint)
	assert.Equal(t, *cluster.S3Endpoint, *deserializedCluster.S3Endpoint)
	require.NotNil(t, deserializedCluster.S3Bucket)
	assert.Equal(t, *cluster.S3Bucket, *deserializedCluster.S3Bucket)
	require.NotNil(t, deserializedCluster.S3Region)
	assert.Equal(t, *cluster.S3Region, *deserializedCluster.S3Region)

	// Verify other fields
	assert.Equal(t, cluster.EnableColumnar, deserializedCluster.EnableColumnar)
	assert.Equal(t, cluster.DefaultShardCount, deserializedCluster.DefaultShardCount)
}

// TestClusterCreateRequest_JSONSerialization tests JSON serialization of request with tiering
func TestClusterCreateRequest_JSONSerialization(t *testing.T) {
	req := &ClusterCreateRequest{
		Name:     "test-cluster",
		Tier:     ClusterTierProduction,
		Provider: CloudProviderAWS,
		Region:   "us-west-2",
		TieringConfig: &TieringConfig{
			Enabled:         true,
			HotDuration:     "7d",
			WarmDuration:    "30d",
			ColdDuration:    "90d",
			CompressionType: "zstd",
		},
		S3Config: &S3Config{
			Endpoint:        "s3.amazonaws.com",
			Bucket:          "orochi-cold",
			Region:          "us-west-2",
			AccessKeyID:     "AKIAIOSFODNN7EXAMPLE",
			SecretAccessKey: "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
		},
		EnableColumnar:    true,
		DefaultShardCount: 16,
	}

	// Serialize to JSON
	jsonData, err := json.Marshal(req)
	require.NoError(t, err, "Failed to marshal request to JSON")

	// Deserialize from JSON
	var deserializedReq ClusterCreateRequest
	err = json.Unmarshal(jsonData, &deserializedReq)
	require.NoError(t, err, "Failed to unmarshal request from JSON")

	// Verify tiering config
	require.NotNil(t, deserializedReq.TieringConfig)
	assert.Equal(t, req.TieringConfig.Enabled, deserializedReq.TieringConfig.Enabled)
	assert.Equal(t, req.TieringConfig.HotDuration, deserializedReq.TieringConfig.HotDuration)
	assert.Equal(t, req.TieringConfig.WarmDuration, deserializedReq.TieringConfig.WarmDuration)
	assert.Equal(t, req.TieringConfig.ColdDuration, deserializedReq.TieringConfig.ColdDuration)
	assert.Equal(t, req.TieringConfig.CompressionType, deserializedReq.TieringConfig.CompressionType)

	// Verify S3 config (note: SecretAccessKey should be omitted in real responses, but present in requests)
	require.NotNil(t, deserializedReq.S3Config)
	assert.Equal(t, req.S3Config.Endpoint, deserializedReq.S3Config.Endpoint)
	assert.Equal(t, req.S3Config.Bucket, deserializedReq.S3Config.Bucket)
	assert.Equal(t, req.S3Config.Region, deserializedReq.S3Config.Region)
	assert.Equal(t, req.S3Config.AccessKeyID, deserializedReq.S3Config.AccessKeyID)

	// Verify other fields
	assert.Equal(t, req.EnableColumnar, deserializedReq.EnableColumnar)
	assert.Equal(t, req.DefaultShardCount, deserializedReq.DefaultShardCount)
}
