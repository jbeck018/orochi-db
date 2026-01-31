// Package cloudnativepg provides integration with the CloudNativePG operator.
package cloudnativepg

import (
	"testing"

	"go.uber.org/zap"
	"k8s.io/apimachinery/pkg/apis/meta/v1/unstructured"

	"github.com/orochi-db/orochi-db/services/provisioner/pkg/config"
	"github.com/orochi-db/orochi-db/services/provisioner/pkg/types"
)

func TestBuildPostgresParameters_Tiering(t *testing.T) {
	logger := zap.NewNop()
	cfg := &config.CloudNativePGConfig{
		DefaultPostgresVersion: "18",
		DefaultStorageClass:    "standard",
	}
	manager := NewClusterManager(nil, cfg, logger)

	tests := []struct {
		name     string
		spec     *types.ClusterSpec
		expected map[string]string
	}{
		{
			name: "tiering disabled",
			spec: &types.ClusterSpec{
				Name:      "test",
				Namespace: "default",
				OrochiConfig: &types.OrochiConfig{
					Enabled: true,
					Tiering: &types.TieringConfig{
						Enabled: false,
					},
				},
			},
			// Note: shared_preload_libraries is now set at postgresql level, not in parameters
			expected: map[string]string{
				"max_connections": "200", // Just verify base params exist
			},
		},
		{
			name: "tiering enabled with all durations",
			spec: &types.ClusterSpec{
				Name:      "test",
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
							Bucket:   "test-bucket",
							Region:   "us-east-1",
						},
					},
				},
			},
			// Note: shared_preload_libraries is now set at postgresql level, not in parameters
			expected: map[string]string{
				"orochi.tiering_enabled":       "on",
				"orochi.tiering_hot_duration":  "7 days",
				"orochi.tiering_warm_duration": "30 days",
				"orochi.tiering_cold_duration": "90 days",
				"orochi.s3_endpoint":           "s3.amazonaws.com",
				"orochi.s3_bucket":             "test-bucket",
				"orochi.s3_region":             "us-east-1",
			},
		},
		{
			name: "tiering with minimal config",
			spec: &types.ClusterSpec{
				Name:      "test",
				Namespace: "default",
				OrochiConfig: &types.OrochiConfig{
					Enabled: true,
					Tiering: &types.TieringConfig{
						Enabled: true,
						S3Config: &types.S3Config{
							Endpoint: "minio.internal:9000",
							Bucket:   "orochi-cold",
						},
					},
				},
			},
			// Note: shared_preload_libraries is now set at postgresql level, not in parameters
			expected: map[string]string{
				"orochi.tiering_enabled": "on",
				"orochi.s3_endpoint":     "minio.internal:9000",
				"orochi.s3_bucket":       "orochi-cold",
			},
		},
		{
			name: "tiering with custom parameters override",
			spec: &types.ClusterSpec{
				Name:      "test",
				Namespace: "default",
				OrochiConfig: &types.OrochiConfig{
					Enabled: true,
					Tiering: &types.TieringConfig{
						Enabled:     true,
						HotDuration: "7 days",
						S3Config: &types.S3Config{
							Endpoint: "s3.amazonaws.com",
							Bucket:   "test-bucket",
						},
					},
					CustomParameters: map[string]string{
						"orochi.tiering_hot_duration": "3 days", // Override
						"orochi.custom_param":         "value",
					},
				},
			},
			// Note: shared_preload_libraries is now set at postgresql level, not in parameters
			expected: map[string]string{
				"orochi.tiering_enabled":      "on",
				"orochi.tiering_hot_duration": "3 days", // Overridden
				"orochi.s3_endpoint":          "s3.amazonaws.com",
				"orochi.s3_bucket":            "test-bucket",
				"orochi.custom_param":         "value",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			params := manager.buildPostgresParameters(tt.spec)

			// Check expected parameters are present
			for key, expectedValue := range tt.expected {
				actualValue, ok := params[key]
				if !ok {
					t.Errorf("expected parameter %s not found", key)
					continue
				}
				if actualValue != expectedValue {
					t.Errorf("parameter %s = %v, want %v", key, actualValue, expectedValue)
				}
			}

			// Check that tiering parameters are NOT present when disabled
			if tt.spec.OrochiConfig != nil && tt.spec.OrochiConfig.Tiering != nil && !tt.spec.OrochiConfig.Tiering.Enabled {
				tieringParams := []string{
					"orochi.tiering_enabled",
					"orochi.tiering_hot_duration",
					"orochi.tiering_warm_duration",
					"orochi.tiering_cold_duration",
					"orochi.s3_endpoint",
					"orochi.s3_bucket",
					"orochi.s3_region",
				}
				for _, param := range tieringParams {
					if _, ok := params[param]; ok {
						t.Errorf("parameter %s should not be present when tiering is disabled", param)
					}
				}
			}
		})
	}
}

func TestBuildClusterSpec_TieringBootstrap(t *testing.T) {
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
					Region:          "us-east-1",
					AccessKeySecret: "orochi-s3-creds",
				},
			},
		},
	}

	cluster := manager.buildClusterSpec(spec)

	// Verify bootstrap SQL is present
	bootstrap, found, err := unstructured.NestedMap(cluster.Object, "spec", "bootstrap")
	if err != nil {
		t.Fatalf("error getting bootstrap: %v", err)
	}
	if !found {
		t.Fatal("bootstrap section not found")
	}

	initdb, found, err := unstructured.NestedMap(bootstrap, "initdb")
	if err != nil {
		t.Fatalf("error getting initdb: %v", err)
	}
	if !found {
		t.Fatal("initdb section not found")
	}

	postInitSQL, found, err := unstructured.NestedSlice(initdb, "postInitSQL")
	if err != nil {
		t.Fatalf("error getting postInitSQL: %v", err)
	}
	if !found {
		t.Fatal("postInitSQL not found")
	}

	// Verify expected SQL statements
	expectedSQL := []string{
		"CREATE EXTENSION IF NOT EXISTS orochi",
		"SELECT orochi.initialize()",
	}

	if len(postInitSQL) != len(expectedSQL) {
		t.Fatalf("postInitSQL length = %d, want %d", len(postInitSQL), len(expectedSQL))
	}

	for i, expectedStmt := range expectedSQL {
		actualStmt, ok := postInitSQL[i].(string)
		if !ok {
			t.Fatalf("postInitSQL[%d] is not a string", i)
		}
		if actualStmt != expectedStmt {
			t.Errorf("postInitSQL[%d] = %q, want %q", i, actualStmt, expectedStmt)
		}
	}

	// Verify environment variables for S3 credentials
	env, found, err := unstructured.NestedSlice(cluster.Object, "spec", "env")
	if err != nil {
		t.Fatalf("error getting env: %v", err)
	}
	if !found {
		t.Fatal("env section not found")
	}

	if len(env) != 2 {
		t.Fatalf("env length = %d, want 2", len(env))
	}

	// Verify AWS_ACCESS_KEY_ID
	env0, ok := env[0].(map[string]interface{})
	if !ok {
		t.Fatal("env[0] is not a map")
	}
	name0, _, _ := unstructured.NestedString(env0, "name")
	if name0 != "AWS_ACCESS_KEY_ID" {
		t.Errorf("env[0].name = %s, want AWS_ACCESS_KEY_ID", name0)
	}

	secretName0, _, _ := unstructured.NestedString(env0, "valueFrom", "secretKeyRef", "name")
	if secretName0 != "orochi-s3-creds" {
		t.Errorf("env[0] secret name = %s, want orochi-s3-creds", secretName0)
	}

	// Verify AWS_SECRET_ACCESS_KEY
	env1, ok := env[1].(map[string]interface{})
	if !ok {
		t.Fatal("env[1] is not a map")
	}
	name1, _, _ := unstructured.NestedString(env1, "name")
	if name1 != "AWS_SECRET_ACCESS_KEY" {
		t.Errorf("env[1].name = %s, want AWS_SECRET_ACCESS_KEY", name1)
	}
}

func TestBuildClusterSpec_NoTieringNoEnvVars(t *testing.T) {
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

	// Verify NO environment variables are added when tiering is disabled
	_, found, err := unstructured.NestedSlice(cluster.Object, "spec", "env")
	if err != nil {
		t.Fatalf("error getting env: %v", err)
	}
	if found {
		t.Error("env section should not be present when tiering is disabled")
	}

	// Verify bootstrap SQL is still present (extension creation is always done)
	bootstrap, found, err := unstructured.NestedMap(cluster.Object, "spec", "bootstrap")
	if err != nil {
		t.Fatalf("error getting bootstrap: %v", err)
	}
	if !found {
		t.Fatal("bootstrap section should always be present")
	}

	initdb, found, err := unstructured.NestedMap(bootstrap, "initdb")
	if err != nil {
		t.Fatalf("error getting initdb: %v", err)
	}
	if !found {
		t.Fatal("initdb section should be present")
	}

	postInitSQL, found, err := unstructured.NestedSlice(initdb, "postInitSQL")
	if err != nil {
		t.Fatalf("error getting postInitSQL: %v", err)
	}
	if !found {
		t.Fatal("postInitSQL should be present")
	}

	if len(postInitSQL) != 2 {
		t.Errorf("postInitSQL length = %d, want 2", len(postInitSQL))
	}
}
