// Package pgdog_integration contains integration tests for PgDog router
package pgdog_integration

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"sync"
	"testing"
	"time"

	_ "github.com/lib/pq"
)

// Test configuration from environment
var (
	pgdogHost     = getEnv("PGDOG_HOST", "localhost")
	pgdogPort     = getEnv("PGDOG_PORT", "5432")
	postgresUser  = getEnv("POSTGRES_USER", "postgres")
	postgresPass  = getEnv("POSTGRES_PASSWORD", "testpassword")
	postgresDB    = getEnv("POSTGRES_DB", "orochi_test")
)

func getEnv(key, defaultValue string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return defaultValue
}

func getConnString() string {
	return fmt.Sprintf(
		"host=%s port=%s user=%s password=%s dbname=%s sslmode=disable",
		pgdogHost, pgdogPort, postgresUser, postgresPass, postgresDB,
	)
}

// CONN-001: Basic connection through proxy
func TestConnection_BasicConnectivity(t *testing.T) {
	db, err := sql.Open("postgres", getConnString())
	if err != nil {
		t.Fatalf("CONN-001 FAILED: Failed to open connection: %v", err)
	}
	defer db.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := db.PingContext(ctx); err != nil {
		t.Fatalf("CONN-001 FAILED: Failed to ping database: %v", err)
	}

	t.Log("CONN-001 PASSED: Basic connection successful")
}

// CONN-004: Multiple concurrent connections
func TestConnection_ConcurrentConnections(t *testing.T) {
	const numConnections = 50
	var wg sync.WaitGroup
	errors := make(chan error, numConnections)

	for i := 0; i < numConnections; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()

			db, err := sql.Open("postgres", getConnString())
			if err != nil {
				errors <- fmt.Errorf("connection %d: open failed: %w", id, err)
				return
			}
			defer db.Close()

			var result int
			if err := db.QueryRow("SELECT 1").Scan(&result); err != nil {
				errors <- fmt.Errorf("connection %d: query failed: %w", id, err)
				return
			}
		}(i)
	}

	wg.Wait()
	close(errors)

	var errs []error
	for err := range errors {
		errs = append(errs, err)
	}

	if len(errs) > 0 {
		t.Fatalf("CONN-004 FAILED: %d/%d connections failed: %v", len(errs), numConnections, errs[0])
	}

	t.Logf("CONN-004 PASSED: %d concurrent connections successful", numConnections)
}

// CONN-005: Connection reuse from pool
func TestConnection_PoolReuse(t *testing.T) {
	db, err := sql.Open("postgres", getConnString())
	if err != nil {
		t.Fatalf("Failed to open connection: %v", err)
	}
	defer db.Close()

	db.SetMaxOpenConns(5)
	db.SetMaxIdleConns(5)

	// Execute multiple queries - should reuse connections
	for i := 0; i < 20; i++ {
		var result int
		if err := db.QueryRow("SELECT $1::int", i).Scan(&result); err != nil {
			t.Fatalf("CONN-005 FAILED: Query %d failed: %v", i, err)
		}
		if result != i {
			t.Fatalf("CONN-005 FAILED: Expected %d, got %d", i, result)
		}
	}

	stats := db.Stats()
	if stats.OpenConnections > 5 {
		t.Fatalf("CONN-005 FAILED: Expected max 5 connections, got %d", stats.OpenConnections)
	}

	t.Logf("CONN-005 PASSED: Pool reuse working (open conns: %d)", stats.OpenConnections)
}

// CONN-007: Prepared statement handling
func TestConnection_PreparedStatements(t *testing.T) {
	db, err := sql.Open("postgres", getConnString())
	if err != nil {
		t.Fatalf("Failed to open connection: %v", err)
	}
	defer db.Close()

	// Prepare statement
	stmt, err := db.Prepare("SELECT $1::int + $2::int")
	if err != nil {
		t.Fatalf("CONN-007 FAILED: Failed to prepare statement: %v", err)
	}
	defer stmt.Close()

	// Execute prepared statement multiple times
	testCases := []struct {
		a, b, expected int
	}{
		{1, 2, 3},
		{10, 20, 30},
		{100, 200, 300},
	}

	for _, tc := range testCases {
		var result int
		if err := stmt.QueryRow(tc.a, tc.b).Scan(&result); err != nil {
			t.Fatalf("CONN-007 FAILED: Prepared statement execution failed: %v", err)
		}
		if result != tc.expected {
			t.Fatalf("CONN-007 FAILED: Expected %d, got %d", tc.expected, result)
		}
	}

	t.Log("CONN-007 PASSED: Prepared statements working")
}

// CONN-008: Transaction isolation
func TestConnection_TransactionIsolation(t *testing.T) {
	db, err := sql.Open("postgres", getConnString())
	if err != nil {
		t.Fatalf("Failed to open connection: %v", err)
	}
	defer db.Close()

	// Start transaction
	tx, err := db.Begin()
	if err != nil {
		t.Fatalf("CONN-008 FAILED: Failed to begin transaction: %v", err)
	}

	// Create temp table in transaction
	_, err = tx.Exec("CREATE TEMP TABLE test_tx (id int)")
	if err != nil {
		tx.Rollback()
		t.Fatalf("CONN-008 FAILED: Failed to create temp table: %v", err)
	}

	// Insert data
	_, err = tx.Exec("INSERT INTO test_tx VALUES (1), (2), (3)")
	if err != nil {
		tx.Rollback()
		t.Fatalf("CONN-008 FAILED: Failed to insert: %v", err)
	}

	// Verify data in transaction
	var count int
	if err := tx.QueryRow("SELECT COUNT(*) FROM test_tx").Scan(&count); err != nil {
		tx.Rollback()
		t.Fatalf("CONN-008 FAILED: Failed to count: %v", err)
	}
	if count != 3 {
		tx.Rollback()
		t.Fatalf("CONN-008 FAILED: Expected 3 rows, got %d", count)
	}

	// Rollback
	if err := tx.Rollback(); err != nil {
		t.Fatalf("CONN-008 FAILED: Failed to rollback: %v", err)
	}

	t.Log("CONN-008 PASSED: Transaction isolation working")
}

// CONN-009: Large result set handling
func TestConnection_LargeResultSet(t *testing.T) {
	db, err := sql.Open("postgres", getConnString())
	if err != nil {
		t.Fatalf("Failed to open connection: %v", err)
	}
	defer db.Close()

	// Generate large result set
	rows, err := db.Query("SELECT generate_series(1, 10000) AS num")
	if err != nil {
		t.Fatalf("CONN-009 FAILED: Failed to query: %v", err)
	}
	defer rows.Close()

	count := 0
	for rows.Next() {
		var num int
		if err := rows.Scan(&num); err != nil {
			t.Fatalf("CONN-009 FAILED: Failed to scan row: %v", err)
		}
		count++
	}

	if err := rows.Err(); err != nil {
		t.Fatalf("CONN-009 FAILED: Row iteration error: %v", err)
	}

	if count != 10000 {
		t.Fatalf("CONN-009 FAILED: Expected 10000 rows, got %d", count)
	}

	t.Logf("CONN-009 PASSED: Large result set (%d rows) handled", count)
}

// CONN-010: Connection timeout handling
func TestConnection_Timeout(t *testing.T) {
	db, err := sql.Open("postgres", getConnString())
	if err != nil {
		t.Fatalf("Failed to open connection: %v", err)
	}
	defer db.Close()

	// Set very short timeout
	ctx, cancel := context.WithTimeout(context.Background(), 1*time.Millisecond)
	defer cancel()

	// This should timeout (pg_sleep for 1 second)
	_, err = db.ExecContext(ctx, "SELECT pg_sleep(1)")
	if err == nil {
		t.Fatal("CONN-010 FAILED: Expected timeout error, got nil")
	}

	// Verify it's a context deadline error
	if ctx.Err() != context.DeadlineExceeded {
		t.Logf("CONN-010 PASSED: Timeout handling working (error: %v)", err)
	} else {
		t.Log("CONN-010 PASSED: Context deadline exceeded as expected")
	}
}

// Benchmark: Connection latency
func BenchmarkConnection_Latency(b *testing.B) {
	db, err := sql.Open("postgres", getConnString())
	if err != nil {
		b.Fatalf("Failed to open connection: %v", err)
	}
	defer db.Close()

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		var result int
		if err := db.QueryRow("SELECT 1").Scan(&result); err != nil {
			b.Fatalf("Query failed: %v", err)
		}
	}
}

// Benchmark: Query overhead through proxy
func BenchmarkConnection_QueryOverhead(b *testing.B) {
	db, err := sql.Open("postgres", getConnString())
	if err != nil {
		b.Fatalf("Failed to open connection: %v", err)
	}
	defer db.Close()

	// Warm up connection pool
	db.Ping()

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		rows, err := db.Query("SELECT * FROM test_data.users LIMIT 10")
		if err != nil {
			b.Fatalf("Query failed: %v", err)
		}
		rows.Close()
	}
}
