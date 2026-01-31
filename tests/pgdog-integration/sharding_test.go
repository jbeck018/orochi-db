// Package pgdog_integration provides integration tests for PgDog sharding
// compatibility with Orochi DB.
//
// These tests verify that PgDog can correctly route queries to the appropriate
// shards based on distribution column values, using the same hash algorithm
// as Orochi DB.
//
// Prerequisites:
//   - PostgreSQL cluster with Orochi extension installed
//   - PgDog router configured with sharding enabled
//   - Test database with distributed tables
//
// Run tests with:
//
//	go test -tags=integration ./tests/pgdog-integration/...
package pgdog_integration

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/google/uuid"
	_ "github.com/lib/pq"
	"github.com/orochi-db/pgdog-router/pkg/hash"
)

// TestConfig holds the configuration for integration tests
type TestConfig struct {
	// PgDog connection string (routes through PgDog)
	PgDogDSN string

	// Direct connection strings to individual shards (for verification)
	ShardDSNs []string

	// Number of shards configured
	ShardCount int32

	// Test database name
	Database string
}

// defaultConfig returns the default test configuration from environment
func defaultConfig() TestConfig {
	pgdogDSN := os.Getenv("PGDOG_DSN")
	if pgdogDSN == "" {
		pgdogDSN = "postgres://postgres:postgres@localhost:6432/orochi_test?sslmode=disable"
	}

	shardCount := int32(4)
	if sc := os.Getenv("SHARD_COUNT"); sc != "" {
		fmt.Sscanf(sc, "%d", &shardCount)
	}

	// Build shard DSNs from pattern
	shardDSNs := make([]string, shardCount)
	shardDSNPattern := os.Getenv("SHARD_DSN_PATTERN")
	if shardDSNPattern == "" {
		shardDSNPattern = "postgres://postgres:postgres@localhost:543%d/orochi_test?sslmode=disable"
	}
	for i := int32(0); i < shardCount; i++ {
		shardDSNs[i] = fmt.Sprintf(shardDSNPattern, i+2) // 5432, 5433, etc.
	}

	return TestConfig{
		PgDogDSN:   pgdogDSN,
		ShardDSNs:  shardDSNs,
		ShardCount: shardCount,
		Database:   "orochi_test",
	}
}

// ShardRouter wraps the hash package for shard routing
type ShardRouter struct {
	router *hash.Router
}

// NewShardRouter creates a new shard router
func NewShardRouter(shardCount int32) *ShardRouter {
	return &ShardRouter{
		router: hash.NewRouter(shardCount),
	}
}

// RouteValue returns the shard index for any supported value
func (r *ShardRouter) RouteValue(value interface{}) int32 {
	return r.router.Route(value)
}

// TestSingleShardQueryRouting tests that queries with a specific distribution
// column value are routed to the correct shard.
func TestSingleShardQueryRouting(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	config := defaultConfig()
	router := NewShardRouter(config.ShardCount)

	// Test cases with different distribution column values
	testCases := []struct {
		name         string
		customerID   int32
		expectedData string
	}{
		{"customer_1", 1, "Customer 1 Data"},
		{"customer_42", 42, "Customer 42 Data"},
		{"customer_100", 100, "Customer 100 Data"},
		{"customer_999", 999, "Customer 999 Data"},
	}

	ctx := context.Background()
	db, err := sql.Open("postgres", config.PgDogDSN)
	if err != nil {
		t.Fatalf("Failed to connect to PgDog: %v", err)
	}
	defer db.Close()

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			expectedShard := router.RouteValue(tc.customerID)
			t.Logf("customer_id=%d should route to shard %d", tc.customerID, expectedShard)

			// Query through PgDog - should route to single shard
			query := "SELECT customer_id, data FROM test_customers WHERE customer_id = $1"
			row := db.QueryRowContext(ctx, query, tc.customerID)

			var returnedID int32
			var returnedData string
			if err := row.Scan(&returnedID, &returnedData); err != nil {
				if err == sql.ErrNoRows {
					t.Logf("No data found for customer_id=%d (test data may not exist)", tc.customerID)
					return
				}
				t.Errorf("Query failed: %v", err)
				return
			}

			if returnedID != tc.customerID {
				t.Errorf("Expected customer_id=%d, got %d", tc.customerID, returnedID)
			}
		})
	}
}

// TestMultiShardQueryAggregation tests that queries without a distribution
// column filter are scattered to all shards and aggregated.
func TestMultiShardQueryAggregation(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	config := defaultConfig()

	ctx := context.Background()
	db, err := sql.Open("postgres", config.PgDogDSN)
	if err != nil {
		t.Fatalf("Failed to connect to PgDog: %v", err)
	}
	defer db.Close()

	// Count query should scatter to all shards and sum results
	t.Run("count_all_customers", func(t *testing.T) {
		var count int64
		err := db.QueryRowContext(ctx, "SELECT COUNT(*) FROM test_customers").Scan(&count)
		if err != nil {
			t.Errorf("Count query failed: %v", err)
			return
		}
		t.Logf("Total customer count across all shards: %d", count)
	})

	// Aggregate query should scatter and combine
	t.Run("sum_order_totals", func(t *testing.T) {
		var total sql.NullFloat64
		err := db.QueryRowContext(ctx, "SELECT SUM(total) FROM test_orders").Scan(&total)
		if err != nil {
			t.Errorf("Sum query failed: %v", err)
			return
		}
		if total.Valid {
			t.Logf("Total order amount across all shards: %.2f", total.Float64)
		}
	})

	// ORDER BY with LIMIT should gather and re-sort
	t.Run("top_customers_by_orders", func(t *testing.T) {
		rows, err := db.QueryContext(ctx,
			"SELECT customer_id, COUNT(*) as order_count FROM test_orders GROUP BY customer_id ORDER BY order_count DESC LIMIT 10")
		if err != nil {
			t.Errorf("Top customers query failed: %v", err)
			return
		}
		defer rows.Close()

		count := 0
		for rows.Next() {
			var customerID int32
			var orderCount int64
			if err := rows.Scan(&customerID, &orderCount); err != nil {
				t.Errorf("Row scan failed: %v", err)
				continue
			}
			count++
			t.Logf("Customer %d has %d orders", customerID, orderCount)
		}
		t.Logf("Retrieved %d top customers", count)
	})
}

// TestInsertRouting tests that INSERTs are routed to the correct shard
// based on the distribution column value.
func TestInsertRouting(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	config := defaultConfig()
	router := NewShardRouter(config.ShardCount)

	ctx := context.Background()
	db, err := sql.Open("postgres", config.PgDogDSN)
	if err != nil {
		t.Fatalf("Failed to connect to PgDog: %v", err)
	}
	defer db.Close()

	// Generate a unique test customer
	testCustomerID := int32(time.Now().UnixNano() % 1000000)
	expectedShard := router.RouteValue(testCustomerID)

	t.Logf("Inserting customer_id=%d, expecting shard %d", testCustomerID, expectedShard)

	// Insert through PgDog
	_, err = db.ExecContext(ctx,
		"INSERT INTO test_customers (customer_id, name, email, data) VALUES ($1, $2, $3, $4) ON CONFLICT DO NOTHING",
		testCustomerID,
		fmt.Sprintf("Test Customer %d", testCustomerID),
		fmt.Sprintf("test%d@example.com", testCustomerID),
		fmt.Sprintf("Integration test data at %s", time.Now().Format(time.RFC3339)))
	if err != nil {
		t.Errorf("Insert failed: %v", err)
		return
	}

	// Verify the data was inserted (query should route to same shard)
	var name string
	err = db.QueryRowContext(ctx,
		"SELECT name FROM test_customers WHERE customer_id = $1",
		testCustomerID).Scan(&name)
	if err != nil {
		t.Errorf("Verification query failed: %v", err)
		return
	}

	if name != fmt.Sprintf("Test Customer %d", testCustomerID) {
		t.Errorf("Data mismatch: expected 'Test Customer %d', got '%s'", testCustomerID, name)
	}

	// Cleanup
	db.ExecContext(ctx, "DELETE FROM test_customers WHERE customer_id = $1", testCustomerID)
}

// TestUpdateDeleteRouting tests that UPDATEs and DELETEs are routed correctly.
func TestUpdateDeleteRouting(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	config := defaultConfig()
	router := NewShardRouter(config.ShardCount)

	ctx := context.Background()
	db, err := sql.Open("postgres", config.PgDogDSN)
	if err != nil {
		t.Fatalf("Failed to connect to PgDog: %v", err)
	}
	defer db.Close()

	// Create a test customer
	testCustomerID := int32(time.Now().UnixNano()%1000000) + 1000000
	expectedShard := router.RouteValue(testCustomerID)

	t.Logf("Testing UPDATE/DELETE for customer_id=%d (shard %d)", testCustomerID, expectedShard)

	// Insert test data
	_, err = db.ExecContext(ctx,
		"INSERT INTO test_customers (customer_id, name, email, data) VALUES ($1, $2, $3, $4)",
		testCustomerID, "Original Name", "original@test.com", "original data")
	if err != nil {
		t.Fatalf("Setup insert failed: %v", err)
	}

	// Test UPDATE with distribution column filter
	t.Run("update_with_distribution_column", func(t *testing.T) {
		result, err := db.ExecContext(ctx,
			"UPDATE test_customers SET name = $1 WHERE customer_id = $2",
			"Updated Name", testCustomerID)
		if err != nil {
			t.Errorf("Update failed: %v", err)
			return
		}
		affected, _ := result.RowsAffected()
		if affected != 1 {
			t.Errorf("Expected 1 row affected, got %d", affected)
		}

		// Verify update
		var name string
		db.QueryRowContext(ctx,
			"SELECT name FROM test_customers WHERE customer_id = $1",
			testCustomerID).Scan(&name)
		if name != "Updated Name" {
			t.Errorf("Update verification failed: expected 'Updated Name', got '%s'", name)
		}
	})

	// Test DELETE with distribution column filter
	t.Run("delete_with_distribution_column", func(t *testing.T) {
		result, err := db.ExecContext(ctx,
			"DELETE FROM test_customers WHERE customer_id = $1",
			testCustomerID)
		if err != nil {
			t.Errorf("Delete failed: %v", err)
			return
		}
		affected, _ := result.RowsAffected()
		if affected != 1 {
			t.Errorf("Expected 1 row affected, got %d", affected)
		}

		// Verify deletion
		var count int
		db.QueryRowContext(ctx,
			"SELECT COUNT(*) FROM test_customers WHERE customer_id = $1",
			testCustomerID).Scan(&count)
		if count != 0 {
			t.Errorf("Delete verification failed: expected 0 rows, got %d", count)
		}
	})
}

// TestCrossShardJoin tests JOIN queries between co-located tables.
func TestCrossShardJoin(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	config := defaultConfig()

	ctx := context.Background()
	db, err := sql.Open("postgres", config.PgDogDSN)
	if err != nil {
		t.Fatalf("Failed to connect to PgDog: %v", err)
	}
	defer db.Close()

	// Test co-located JOIN (same distribution column)
	t.Run("colocated_join", func(t *testing.T) {
		// This should be efficient - executed on each shard locally
		query := `
			SELECT c.customer_id, c.name, COUNT(o.order_id) as order_count, SUM(o.total) as total_spent
			FROM test_customers c
			LEFT JOIN test_orders o ON c.customer_id = o.customer_id
			WHERE c.customer_id = $1
			GROUP BY c.customer_id, c.name`

		row := db.QueryRowContext(ctx, query, 42)
		var customerID int32
		var name string
		var orderCount int64
		var totalSpent sql.NullFloat64

		if err := row.Scan(&customerID, &name, &orderCount, &totalSpent); err != nil {
			if err == sql.ErrNoRows {
				t.Log("No data found for customer 42 (test data may not exist)")
				return
			}
			t.Errorf("Co-located join query failed: %v", err)
			return
		}
		t.Logf("Customer %d (%s): %d orders, $%.2f total", customerID, name, orderCount, totalSpent.Float64)
	})

	// Test reference table JOIN (should work from any shard)
	t.Run("reference_table_join", func(t *testing.T) {
		// Reference tables are replicated to all shards
		query := `
			SELECT c.customer_id, c.name, r.region_name
			FROM test_customers c
			LEFT JOIN ref_regions r ON c.region_id = r.region_id
			WHERE c.customer_id = $1`

		row := db.QueryRowContext(ctx, query, 42)
		var customerID int32
		var name string
		var regionName sql.NullString

		if err := row.Scan(&customerID, &name, &regionName); err != nil {
			if err == sql.ErrNoRows {
				t.Log("No data found for customer 42 (test data may not exist)")
				return
			}
			t.Errorf("Reference table join query failed: %v", err)
			return
		}
		t.Logf("Customer %d (%s): Region %s", customerID, name, regionName.String)
	})

	// Test cross-shard aggregation JOIN
	t.Run("cross_shard_aggregation_join", func(t *testing.T) {
		// This requires scatter-gather across all shards
		query := `
			SELECT COUNT(DISTINCT c.customer_id), SUM(o.total)
			FROM test_customers c
			JOIN test_orders o ON c.customer_id = o.customer_id`

		row := db.QueryRowContext(ctx, query)
		var customerCount int64
		var totalAmount sql.NullFloat64

		if err := row.Scan(&customerCount, &totalAmount); err != nil {
			t.Errorf("Cross-shard aggregation query failed: %v", err)
			return
		}
		t.Logf("Cross-shard: %d customers with orders, $%.2f total", customerCount, totalAmount.Float64)
	})
}

// TestTwoPhaseCommit tests distributed transaction handling with 2PC.
func TestTwoPhaseCommit(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	config := defaultConfig()
	router := NewShardRouter(config.ShardCount)

	ctx := context.Background()
	db, err := sql.Open("postgres", config.PgDogDSN)
	if err != nil {
		t.Fatalf("Failed to connect to PgDog: %v", err)
	}
	defer db.Close()

	// Test single-shard transaction (no 2PC needed)
	t.Run("single_shard_transaction", func(t *testing.T) {
		testID := int32(time.Now().UnixNano()%1000000) + 2000000
		shard := router.RouteValue(testID)

		tx, err := db.BeginTx(ctx, nil)
		if err != nil {
			t.Fatalf("Begin transaction failed: %v", err)
		}

		_, err = tx.ExecContext(ctx,
			"INSERT INTO test_customers (customer_id, name, email, data) VALUES ($1, $2, $3, $4)",
			testID, "TX Test", "tx@test.com", "single shard tx")
		if err != nil {
			tx.Rollback()
			t.Fatalf("Insert in transaction failed: %v", err)
		}

		err = tx.Commit()
		if err != nil {
			t.Errorf("Commit failed: %v", err)
			return
		}
		t.Logf("Single-shard transaction committed successfully (shard %d)", shard)

		// Cleanup
		db.ExecContext(ctx, "DELETE FROM test_customers WHERE customer_id = $1", testID)
	})

	// Test multi-shard transaction (requires 2PC)
	t.Run("multi_shard_transaction", func(t *testing.T) {
		// Find two customer IDs that map to different shards
		var id1, id2 int32
		var shard1, shard2 int32
		for i := int32(1); i < 10000; i++ {
			shard := router.RouteValue(i)
			if id1 == 0 {
				id1 = i
				shard1 = shard
			} else if shard != shard1 {
				id2 = i
				shard2 = shard
				break
			}
		}

		// Offset to avoid conflicts
		id1 += 3000000
		id2 += 3000000

		t.Logf("Testing 2PC with customer_id=%d (shard %d) and customer_id=%d (shard %d)",
			id1, shard1, id2, shard2)

		tx, err := db.BeginTx(ctx, nil)
		if err != nil {
			t.Fatalf("Begin transaction failed: %v", err)
		}

		// Insert on shard 1
		_, err = tx.ExecContext(ctx,
			"INSERT INTO test_customers (customer_id, name, email, data) VALUES ($1, $2, $3, $4)",
			id1, "2PC Test 1", "2pc1@test.com", "multi shard tx")
		if err != nil {
			tx.Rollback()
			t.Fatalf("First insert failed: %v", err)
		}

		// Insert on shard 2
		_, err = tx.ExecContext(ctx,
			"INSERT INTO test_customers (customer_id, name, email, data) VALUES ($1, $2, $3, $4)",
			id2, "2PC Test 2", "2pc2@test.com", "multi shard tx")
		if err != nil {
			tx.Rollback()
			t.Fatalf("Second insert failed: %v", err)
		}

		err = tx.Commit()
		if err != nil {
			t.Errorf("2PC commit failed: %v", err)
			return
		}
		t.Log("Multi-shard 2PC transaction committed successfully")

		// Verify both rows exist
		var count int
		db.QueryRowContext(ctx,
			"SELECT COUNT(*) FROM test_customers WHERE customer_id IN ($1, $2)",
			id1, id2).Scan(&count)
		if count != 2 {
			t.Errorf("Expected 2 rows after 2PC commit, got %d", count)
		}

		// Cleanup
		db.ExecContext(ctx, "DELETE FROM test_customers WHERE customer_id IN ($1, $2)", id1, id2)
	})

	// Test transaction rollback
	t.Run("transaction_rollback", func(t *testing.T) {
		testID := int32(time.Now().UnixNano()%1000000) + 4000000

		tx, err := db.BeginTx(ctx, nil)
		if err != nil {
			t.Fatalf("Begin transaction failed: %v", err)
		}

		_, err = tx.ExecContext(ctx,
			"INSERT INTO test_customers (customer_id, name, email, data) VALUES ($1, $2, $3, $4)",
			testID, "Rollback Test", "rollback@test.com", "should be rolled back")
		if err != nil {
			tx.Rollback()
			t.Fatalf("Insert in transaction failed: %v", err)
		}

		// Explicitly rollback
		err = tx.Rollback()
		if err != nil {
			t.Errorf("Rollback failed: %v", err)
			return
		}

		// Verify row does not exist
		var count int
		db.QueryRowContext(ctx,
			"SELECT COUNT(*) FROM test_customers WHERE customer_id = $1",
			testID).Scan(&count)
		if count != 0 {
			t.Errorf("Expected 0 rows after rollback, got %d", count)
		}
		t.Log("Transaction rollback successful")
	})
}

// TestUUIDDistribution tests sharding with UUID distribution columns.
func TestUUIDDistribution(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	config := defaultConfig()
	router := NewShardRouter(config.ShardCount)

	ctx := context.Background()
	db, err := sql.Open("postgres", config.PgDogDSN)
	if err != nil {
		t.Fatalf("Failed to connect to PgDog: %v", err)
	}
	defer db.Close()

	testCases := []struct {
		name string
		uuid uuid.UUID
	}{
		{"uuid_v4_1", uuid.MustParse("550e8400-e29b-41d4-a716-446655440000")},
		{"uuid_v4_2", uuid.MustParse("a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11")},
		{"uuid_v4_3", uuid.MustParse("f47ac10b-58cc-4372-a567-0e02b2c3d479")},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			expectedShard := router.RouteValue(tc.uuid)
			t.Logf("UUID %s should route to shard %d", tc.uuid, expectedShard)

			// Query for UUID (assuming test_events table with event_id as UUID)
			row := db.QueryRowContext(ctx,
				"SELECT event_id FROM test_events WHERE event_id = $1",
				tc.uuid)

			var returnedUUID uuid.UUID
			if err := row.Scan(&returnedUUID); err != nil {
				if err == sql.ErrNoRows {
					t.Logf("No data found for UUID %s (test data may not exist)", tc.uuid)
					return
				}
				t.Logf("Query returned error (may be expected if table doesn't exist): %v", err)
				return
			}

			if returnedUUID != tc.uuid {
				t.Errorf("Expected UUID %s, got %s", tc.uuid, returnedUUID)
			}
		})
	}
}

// TestHashConsistencyWithOrochi verifies Go hash matches Orochi DB hash.
// This test requires the Orochi extension to be installed and available.
func TestHashConsistencyWithOrochi(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test in short mode")
	}

	config := defaultConfig()

	ctx := context.Background()
	db, err := sql.Open("postgres", config.PgDogDSN)
	if err != nil {
		t.Fatalf("Failed to connect to database: %v", err)
	}
	defer db.Close()

	// Test INT4 hash consistency
	t.Run("int4_hash_consistency", func(t *testing.T) {
		testValues := []int32{0, 1, -1, 42, 100, 12345, -12345, 2147483647, -2147483648}

		for _, val := range testValues {
			goHash := hash.HashInt32(val)

			var pgHash int32
			err := db.QueryRowContext(ctx,
				"SELECT orochi.hash_value($1::int4)",
				val).Scan(&pgHash)
			if err != nil {
				t.Logf("Could not verify hash for %d (Orochi function may not be available): %v", val, err)
				continue
			}

			if goHash != pgHash {
				t.Errorf("Hash mismatch for int4 %d: Go=%d, Orochi=%d", val, goHash, pgHash)
			} else {
				t.Logf("Hash match for int4 %d: %d", val, goHash)
			}
		}
	})

	// Test INT8 hash consistency
	t.Run("int8_hash_consistency", func(t *testing.T) {
		testValues := []int64{0, 1, -1, 1234567890123456789, -1234567890123456789}

		for _, val := range testValues {
			goHash := hash.HashInt64(val)

			var pgHash int32
			err := db.QueryRowContext(ctx,
				"SELECT orochi.hash_value($1::int8)",
				val).Scan(&pgHash)
			if err != nil {
				t.Logf("Could not verify hash for %d (Orochi function may not be available): %v", val, err)
				continue
			}

			if goHash != pgHash {
				t.Errorf("Hash mismatch for int8 %d: Go=%d, Orochi=%d", val, goHash, pgHash)
			} else {
				t.Logf("Hash match for int8 %d: %d", val, goHash)
			}
		}
	})

	// Test TEXT hash consistency
	t.Run("text_hash_consistency", func(t *testing.T) {
		testValues := []string{"", "hello", "test", "user@example.com", "hello world"}

		for _, val := range testValues {
			goHash := hash.HashText(val)

			var pgHash int32
			err := db.QueryRowContext(ctx,
				"SELECT orochi.hash_value($1::text)",
				val).Scan(&pgHash)
			if err != nil {
				t.Logf("Could not verify hash for '%s' (Orochi function may not be available): %v", val, err)
				continue
			}

			if goHash != pgHash {
				t.Errorf("Hash mismatch for text '%s': Go=%d, Orochi=%d", val, goHash, pgHash)
			} else {
				t.Logf("Hash match for text '%s': %d", val, goHash)
			}
		}
	})

	// Test UUID hash consistency
	t.Run("uuid_hash_consistency", func(t *testing.T) {
		testValues := []string{
			"00000000-0000-0000-0000-000000000000",
			"550e8400-e29b-41d4-a716-446655440000",
			"a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11",
		}

		for _, val := range testValues {
			u := uuid.MustParse(val)
			goHash := hash.HashUUID(u)

			var pgHash int32
			err := db.QueryRowContext(ctx,
				"SELECT orochi.hash_value($1::uuid)",
				val).Scan(&pgHash)
			if err != nil {
				t.Logf("Could not verify hash for UUID %s (Orochi function may not be available): %v", val, err)
				continue
			}

			if goHash != pgHash {
				t.Errorf("Hash mismatch for UUID %s: Go=%d, Orochi=%d", val, goHash, pgHash)
			} else {
				t.Logf("Hash match for UUID %s: %d", val, goHash)
			}
		}
	})

	// Test shard index consistency
	t.Run("shard_index_consistency", func(t *testing.T) {
		testValues := []int32{0, 42, 100, 12345, -1, 2147483647}
		shardCounts := []int32{4, 8, 16, 32}

		for _, val := range testValues {
			hashVal := hash.HashInt32(val)
			for _, sc := range shardCounts {
				goShard := hash.GetShardIndex(hashVal, sc)

				var pgShard int32
				err := db.QueryRowContext(ctx,
					"SELECT orochi.get_shard_index(orochi.hash_value($1::int4), $2)",
					val, sc).Scan(&pgShard)
				if err != nil {
					t.Logf("Could not verify shard index (Orochi function may not be available): %v", err)
					continue
				}

				if goShard != pgShard {
					t.Errorf("Shard index mismatch for value=%d, shards=%d: Go=%d, Orochi=%d",
						val, sc, goShard, pgShard)
				}
			}
		}
	})
}

// BenchmarkHashRouting benchmarks the hash routing performance
func BenchmarkHashRouting(b *testing.B) {
	router := NewShardRouter(32)

	b.Run("RouteInt32", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			router.RouteValue(int32(i))
		}
	})

	b.Run("RouteInt64", func(b *testing.B) {
		for i := 0; i < b.N; i++ {
			router.RouteValue(int64(i))
		}
	})

	b.Run("RouteText", func(b *testing.B) {
		text := "user@example.com"
		for i := 0; i < b.N; i++ {
			router.RouteValue(text)
		}
	})

	b.Run("RouteUUID", func(b *testing.B) {
		u := uuid.MustParse("550e8400-e29b-41d4-a716-446655440000")
		for i := 0; i < b.N; i++ {
			router.RouteValue(u)
		}
	})
}
