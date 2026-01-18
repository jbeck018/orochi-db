package cloudnativepg

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
	"go.uber.org/zap"
	"k8s.io/apimachinery/pkg/apis/meta/v1/unstructured"

	"github.com/orochi-db/orochi-db/services/provisioner/pkg/config"
	"github.com/orochi-db/orochi-db/services/provisioner/pkg/types"
)

// TestBuildPostgresParameters_TieringEnabled tests that tiering GUCs are included when enabled
func TestBuildPostgresParameters_TieringEnabled(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	spec := &types.ClusterSpec{
		Name:      "test-cluster",
		Namespace: "default",
		OrochiConfig: &types.OrochiConfig{
			Enabled:           true,
			DefaultShardCount: 16,
			EnableColumnar:    true,
			Tiering: &types.TieringConfig{
				Enabled:      true,
				HotDuration:  "7 days",
				WarmDuration: "30 days",
				ColdDuration: "90 days",
				S3Config: &types.S3Config{
					Endpoint: "s3.amazonaws.com",
					Bucket:   "orochi-cold-tier",
					Region:   "us-west-2",
				},
			},
		},
	}

	params := manager.buildPostgresParameters(spec)

	// Verify base parameters
	assert.Equal(t, "orochi", params["shared_preload_libraries"], "shared_preload_libraries should be orochi")

	// Verify tiering parameters
	assert.Equal(t, "on", params["orochi.tiering_enabled"], "tiering should be enabled")
	assert.Equal(t, "7 days", params["orochi.tiering_hot_duration"], "hot duration should match")
	assert.Equal(t, "30 days", params["orochi.tiering_warm_duration"], "warm duration should match")
	assert.Equal(t, "90 days", params["orochi.tiering_cold_duration"], "cold duration should match")

	// Verify S3 parameters
	assert.Equal(t, "s3.amazonaws.com", params["orochi.s3_endpoint"], "S3 endpoint should match")
	assert.Equal(t, "orochi-cold-tier", params["orochi.s3_bucket"], "S3 bucket should match")
	assert.Equal(t, "us-west-2", params["orochi.s3_region"], "S3 region should match")

	// Verify other Orochi parameters
	assert.Equal(t, "16", params["orochi.default_shard_count"], "shard count should match")
	assert.Equal(t, "on", params["orochi.enable_columnar"], "columnar should be enabled")
}

// TestBuildPostgresParameters_TieringDisabled tests that tiering GUCs are NOT included when disabled
func TestBuildPostgresParameters_TieringDisabled(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	spec := &types.ClusterSpec{
		Name:      "test-cluster",
		Namespace: "default",
		OrochiConfig: &types.OrochiConfig{
			Enabled: true,
			Tiering: &types.TieringConfig{
				Enabled: false,
			},
		},
	}

	params := manager.buildPostgresParameters(spec)

	// Verify base parameters are present
	assert.Equal(t, "orochi", params["shared_preload_libraries"])

	// Verify tiering parameters are NOT present
	_, hasTieringEnabled := params["orochi.tiering_enabled"]
	assert.False(t, hasTieringEnabled, "tiering_enabled should not be present when disabled")

	_, hasHotDuration := params["orochi.tiering_hot_duration"]
	assert.False(t, hasHotDuration, "hot duration should not be present when disabled")

	_, hasS3Endpoint := params["orochi.s3_endpoint"]
	assert.False(t, hasS3Endpoint, "S3 endpoint should not be present when disabled")

	_, hasS3Bucket := params["orochi.s3_bucket"]
	assert.False(t, hasS3Bucket, "S3 bucket should not be present when disabled")
}

// TestBuildPostgresParameters_NoOrochiConfig tests parameters when Orochi is not enabled
func TestBuildPostgresParameters_NoOrochiConfig(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	spec := &types.ClusterSpec{
		Name:      "test-cluster",
		Namespace: "default",
		// No OrochiConfig
	}

	params := manager.buildPostgresParameters(spec)

	// Should only have base PostgreSQL parameters
	assert.Equal(t, "orochi", params["shared_preload_libraries"])
	assert.Contains(t, params, "max_connections")
	assert.Contains(t, params, "shared_buffers")

	// Should not have any Orochi-specific parameters
	_, hasShardCount := params["orochi.default_shard_count"]
	assert.False(t, hasShardCount, "shard count should not be present without OrochiConfig")
}

// TestBuildPostgresParameters_MinimalTiering tests tiering with minimal S3 config
func TestBuildPostgresParameters_MinimalTiering(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	spec := &types.ClusterSpec{
		Name:      "test-cluster",
		Namespace: "default",
		OrochiConfig: &types.OrochiConfig{
			Enabled: true,
			Tiering: &types.TieringConfig{
				Enabled:      true,
				HotDuration:  "7 days",
				WarmDuration: "30 days",
				ColdDuration: "90 days",
				S3Config: &types.S3Config{
					Endpoint: "minio.internal:9000",
					Bucket:   "orochi-cold",
					// No region specified
				},
			},
		},
	}

	params := manager.buildPostgresParameters(spec)

	// Verify tiering is enabled
	assert.Equal(t, "on", params["orochi.tiering_enabled"])

	// Verify S3 endpoint and bucket
	assert.Equal(t, "minio.internal:9000", params["orochi.s3_endpoint"])
	assert.Equal(t, "orochi-cold", params["orochi.s3_bucket"])

	// Verify S3 region is not present (optional)
	_, hasRegion := params["orochi.s3_region"]
	assert.False(t, hasRegion, "S3 region should not be present when not specified")
}

// TestBuildPostgresParameters_CustomParametersOverride tests that custom parameters can override defaults
func TestBuildPostgresParameters_CustomParametersOverride(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	spec := &types.ClusterSpec{
		Name:      "test-cluster",
		Namespace: "default",
		OrochiConfig: &types.OrochiConfig{
			Enabled: true,
			Tiering: &types.TieringConfig{
				Enabled:      true,
				HotDuration:  "7 days",
				WarmDuration: "30 days",
				ColdDuration: "90 days",
				S3Config: &types.S3Config{
					Endpoint: "s3.amazonaws.com",
					Bucket:   "bucket",
				},
			},
			CustomParameters: map[string]string{
				"orochi.tiering_hot_duration": "3 days", // Override
				"orochi.custom_setting":       "value",
				"max_connections":             "500", // Override base PostgreSQL parameter
			},
		},
	}

	params := manager.buildPostgresParameters(spec)

	// Verify overridden tiering parameter
	assert.Equal(t, "3 days", params["orochi.tiering_hot_duration"], "custom parameters should override")

	// Verify custom parameter is present
	assert.Equal(t, "value", params["orochi.custom_setting"], "custom parameters should be included")

	// Verify overridden base parameter
	assert.Equal(t, "500", params["max_connections"], "custom parameters should override base parameters")

	// Verify other tiering parameters still present
	assert.Equal(t, "on", params["orochi.tiering_enabled"])
	assert.Equal(t, "30 days", params["orochi.tiering_warm_duration"])
}

// TestBuildClusterSpec_BootstrapSQL tests that bootstrap SQL includes CREATE EXTENSION
func TestBuildClusterSpec_BootstrapSQL(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	spec := &types.ClusterSpec{
		Name:      "test-cluster",
		Namespace: "default",
		Instances: 3,
		Resources: types.ResourceRequirements{
			CPURequest:    "1",
			MemoryRequest: "2Gi",
		},
		Storage: types.StorageSpec{
			Size: "10Gi",
		},
		OrochiConfig: &types.OrochiConfig{
			Enabled: true,
		},
	}

	cluster := manager.buildClusterSpec(spec)

	// Verify bootstrap section exists
	bootstrap, found, err := unstructured.NestedMap(cluster.Object, "spec", "bootstrap")
	require.NoError(t, err)
	require.True(t, found, "bootstrap section should exist")

	// Verify initdb section
	initdb, found, err := unstructured.NestedMap(bootstrap, "initdb")
	require.NoError(t, err)
	require.True(t, found, "initdb section should exist")

	// Verify postInitSQL
	postInitSQL, found, err := unstructured.NestedSlice(initdb, "postInitSQL")
	require.NoError(t, err)
	require.True(t, found, "postInitSQL should exist")

	// Verify SQL statements
	require.Len(t, postInitSQL, 2, "should have 2 SQL statements")

	assert.Equal(t, "CREATE EXTENSION IF NOT EXISTS orochi", postInitSQL[0])
	assert.Equal(t, "SELECT orochi.initialize()", postInitSQL[1])
}

// TestBuildClusterSpec_S3EnvironmentVariables tests S3 credentials are set as env vars
func TestBuildClusterSpec_S3EnvironmentVariables(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	spec := &types.ClusterSpec{
		Name:      "test-cluster",
		Namespace: "default",
		Instances: 3,
		Resources: types.ResourceRequirements{
			CPURequest:    "1",
			MemoryRequest: "2Gi",
		},
		Storage: types.StorageSpec{
			Size: "10Gi",
		},
		OrochiConfig: &types.OrochiConfig{
			Enabled: true,
			Tiering: &types.TieringConfig{
				Enabled:      true,
				HotDuration:  "7 days",
				WarmDuration: "30 days",
				ColdDuration: "90 days",
				S3Config: &types.S3Config{
					Endpoint:        "s3.amazonaws.com",
					Bucket:          "test-bucket",
					Region:          "us-west-2",
					AccessKeySecret: "orochi-s3-credentials",
				},
			},
		},
	}

	cluster := manager.buildClusterSpec(spec)

	// Verify environment variables section exists
	env, found, err := unstructured.NestedSlice(cluster.Object, "spec", "env")
	require.NoError(t, err)
	require.True(t, found, "env section should exist when tiering is enabled")

	// Should have 2 environment variables (AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY)
	require.Len(t, env, 2, "should have 2 environment variables")

	// Verify AWS_ACCESS_KEY_ID
	env0, ok := env[0].(map[string]interface{})
	require.True(t, ok, "env[0] should be a map")

	name0, _, _ := unstructured.NestedString(env0, "name")
	assert.Equal(t, "AWS_ACCESS_KEY_ID", name0)

	secretName0, _, _ := unstructured.NestedString(env0, "valueFrom", "secretKeyRef", "name")
	assert.Equal(t, "orochi-s3-credentials", secretName0)

	secretKey0, _, _ := unstructured.NestedString(env0, "valueFrom", "secretKeyRef", "key")
	assert.Equal(t, "AWS_ACCESS_KEY_ID", secretKey0)

	// Verify AWS_SECRET_ACCESS_KEY
	env1, ok := env[1].(map[string]interface{})
	require.True(t, ok, "env[1] should be a map")

	name1, _, _ := unstructured.NestedString(env1, "name")
	assert.Equal(t, "AWS_SECRET_ACCESS_KEY", name1)

	secretName1, _, _ := unstructured.NestedString(env1, "valueFrom", "secretKeyRef", "name")
	assert.Equal(t, "orochi-s3-credentials", secretName1)

	secretKey1, _, _ := unstructured.NestedString(env1, "valueFrom", "secretKeyRef", "key")
	assert.Equal(t, "AWS_SECRET_ACCESS_KEY", secretKey1)
}

// TestBuildClusterSpec_NoS3EnvVarsWhenTieringDisabled tests no env vars when tiering is disabled
func TestBuildClusterSpec_NoS3EnvVarsWhenTieringDisabled(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	spec := &types.ClusterSpec{
		Name:      "test-cluster",
		Namespace: "default",
		Instances: 3,
		Resources: types.ResourceRequirements{
			CPURequest:    "1",
			MemoryRequest: "2Gi",
		},
		Storage: types.StorageSpec{
			Size: "10Gi",
		},
		OrochiConfig: &types.OrochiConfig{
			Enabled: true,
			// No tiering config
		},
	}

	cluster := manager.buildClusterSpec(spec)

	// Verify NO environment variables section when tiering is disabled
	_, found, err := unstructured.NestedSlice(cluster.Object, "spec", "env")
	require.NoError(t, err)
	assert.False(t, found, "env section should not exist when tiering is disabled")
}

// TestBuildClusterSpec_NoS3EnvVarsWhenNoAccessKeySecret tests no env vars without secret name
func TestBuildClusterSpec_NoS3EnvVarsWhenNoAccessKeySecret(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	spec := &types.ClusterSpec{
		Name:      "test-cluster",
		Namespace: "default",
		Instances: 3,
		Resources: types.ResourceRequirements{
			CPURequest:    "1",
			MemoryRequest: "2Gi",
		},
		Storage: types.StorageSpec{
			Size: "10Gi",
		},
		OrochiConfig: &types.OrochiConfig{
			Enabled: true,
			Tiering: &types.TieringConfig{
				Enabled:      true,
				HotDuration:  "7 days",
				WarmDuration: "30 days",
				ColdDuration: "90 days",
				S3Config: &types.S3Config{
					Endpoint: "s3.amazonaws.com",
					Bucket:   "test-bucket",
					Region:   "us-west-2",
					// No AccessKeySecret specified
				},
			},
		},
	}

	cluster := manager.buildClusterSpec(spec)

	// Verify NO environment variables section when AccessKeySecret is not specified
	_, found, err := unstructured.NestedSlice(cluster.Object, "spec", "env")
	require.NoError(t, err)
	assert.False(t, found, "env section should not exist when AccessKeySecret is not specified")
}

// TestBuildClusterSpec_CompleteConfiguration tests a complete tiering configuration
func TestBuildClusterSpec_CompleteConfiguration(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	spec := &types.ClusterSpec{
		Name:      "production-cluster",
		Namespace: "orochi-prod",
		Instances: 5,
		Resources: types.ResourceRequirements{
			CPURequest:    "4",
			CPULimit:      "8",
			MemoryRequest: "16Gi",
			MemoryLimit:   "32Gi",
		},
		Storage: types.StorageSpec{
			Size:         "500Gi",
			StorageClass: "fast-ssd",
		},
		OrochiConfig: &types.OrochiConfig{
			Enabled:              true,
			DefaultShardCount:    32,
			ChunkInterval:        "1 day",
			DefaultCompression:   types.CompressionZSTD,
			EnableColumnar:       true,
			Tiering: &types.TieringConfig{
				Enabled:      true,
				HotDuration:  "7 days",
				WarmDuration: "30 days",
				ColdDuration: "90 days",
				S3Config: &types.S3Config{
					Endpoint:        "s3.us-west-2.amazonaws.com",
					Bucket:          "orochi-production-cold-tier",
					Region:          "us-west-2",
					AccessKeySecret: "orochi-prod-s3-creds",
				},
			},
		},
	}

	cluster := manager.buildClusterSpec(spec)

	// Verify basic cluster spec
	instances, _, _ := unstructured.NestedInt64(cluster.Object, "spec", "instances")
	assert.Equal(t, int64(5), instances)

	// Verify PostgreSQL parameters include all tiering settings
	params, found, err := unstructured.NestedMap(cluster.Object, "spec", "postgresql", "parameters")
	require.NoError(t, err)
	require.True(t, found)

	assert.Equal(t, "orochi", params["shared_preload_libraries"])
	assert.Equal(t, "on", params["orochi.tiering_enabled"])
	assert.Equal(t, "7 days", params["orochi.tiering_hot_duration"])
	assert.Equal(t, "30 days", params["orochi.tiering_warm_duration"])
	assert.Equal(t, "90 days", params["orochi.tiering_cold_duration"])
	assert.Equal(t, "s3.us-west-2.amazonaws.com", params["orochi.s3_endpoint"])
	assert.Equal(t, "orochi-production-cold-tier", params["orochi.s3_bucket"])
	assert.Equal(t, "us-west-2", params["orochi.s3_region"])
	assert.Equal(t, "32", params["orochi.default_shard_count"])
	assert.Equal(t, "1 day", params["orochi.default_chunk_interval"])
	assert.Equal(t, "zstd", params["orochi.default_compression"])
	assert.Equal(t, "on", params["orochi.enable_columnar"])

	// Verify S3 environment variables
	env, found, err := unstructured.NestedSlice(cluster.Object, "spec", "env")
	require.NoError(t, err)
	require.True(t, found)
	require.Len(t, env, 2)

	// Verify bootstrap SQL
	bootstrap, found, err := unstructured.NestedMap(cluster.Object, "spec", "bootstrap")
	require.NoError(t, err)
	require.True(t, found)

	initdb, found, err := unstructured.NestedMap(bootstrap, "initdb")
	require.NoError(t, err)
	require.True(t, found)

	postInitSQL, found, err := unstructured.NestedSlice(initdb, "postInitSQL")
	require.NoError(t, err)
	require.True(t, found)
	require.Len(t, postInitSQL, 2)
}

// TestBuildClusterSpec_SharedPreloadLibraries tests shared_preload_libraries parameter
func TestBuildClusterSpec_SharedPreloadLibraries(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	spec := &types.ClusterSpec{
		Name:      "test-cluster",
		Namespace: "default",
		Instances: 1,
		Resources: types.ResourceRequirements{
			CPURequest:    "1",
			MemoryRequest: "2Gi",
		},
		Storage: types.StorageSpec{
			Size: "10Gi",
		},
		OrochiConfig: &types.OrochiConfig{
			Enabled: true,
		},
	}

	cluster := manager.buildClusterSpec(spec)

	// Verify shared_preload_libraries in both locations
	preloadLibsArray, found, err := unstructured.NestedSlice(cluster.Object, "spec", "postgresql", "shared_preload_libraries")
	require.NoError(t, err)
	require.True(t, found, "shared_preload_libraries array should exist")
	require.Len(t, preloadLibsArray, 1)
	assert.Equal(t, "orochi", preloadLibsArray[0])

	// Verify in parameters as well
	params, found, err := unstructured.NestedMap(cluster.Object, "spec", "postgresql", "parameters")
	require.NoError(t, err)
	require.True(t, found)
	assert.Equal(t, "orochi", params["shared_preload_libraries"])
}
