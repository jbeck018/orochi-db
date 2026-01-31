package hash

import (
	"fmt"
	"math"
	"testing"

	"github.com/google/uuid"
)

// TestOrochiHash verifies the basic CRC32 hash implementation
func TestOrochiHash(t *testing.T) {
	tests := []struct {
		name     string
		data     []byte
		expected uint32
	}{
		{
			name:     "empty data",
			data:     []byte{},
			expected: 0x00000000,
		},
		{
			name:     "single byte zero",
			data:     []byte{0x00},
			expected: 0xD202EF8D,
		},
		{
			name:     "single byte 0xFF",
			data:     []byte{0xFF},
			expected: 0xFF000000,
		},
		{
			name:     "hello world",
			data:     []byte("hello world"),
			expected: 0x0D4A1185,
		},
		{
			name:     "test string",
			data:     []byte("test"),
			expected: 0xD87F7E0C,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			result := OrochiHash(tt.data)
			if result != tt.expected {
				t.Errorf("OrochiHash(%v) = 0x%08X, want 0x%08X", tt.data, result, tt.expected)
			}
		})
	}
}

// TestHashInt16 tests 16-bit integer hashing
func TestHashInt16(t *testing.T) {
	tests := []struct {
		name  string
		value int16
	}{
		{"zero", 0},
		{"positive", 12345},
		{"negative", -12345},
		{"max", math.MaxInt16},
		{"min", math.MinInt16},
		{"one", 1},
		{"minus one", -1},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			hash := HashInt16(tt.value)
			// Verify hash is deterministic
			hash2 := HashInt16(tt.value)
			if hash != hash2 {
				t.Errorf("HashInt16(%d) not deterministic: %d != %d", tt.value, hash, hash2)
			}
		})
	}
}

// TestHashInt32 tests 32-bit integer hashing
func TestHashInt32(t *testing.T) {
	tests := []struct {
		name  string
		value int32
	}{
		{"zero", 0},
		{"positive", 123456789},
		{"negative", -123456789},
		{"max", math.MaxInt32},
		{"min", math.MinInt32},
		{"one", 1},
		{"minus one", -1},
		{"user_id_1", 1001},
		{"user_id_typical", 42},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			hash := HashInt32(tt.value)
			// Verify hash is deterministic
			hash2 := HashInt32(tt.value)
			if hash != hash2 {
				t.Errorf("HashInt32(%d) not deterministic: %d != %d", tt.value, hash, hash2)
			}
		})
	}
}

// TestHashInt64 tests 64-bit integer hashing
func TestHashInt64(t *testing.T) {
	tests := []struct {
		name  string
		value int64
	}{
		{"zero", 0},
		{"positive", 1234567890123456789},
		{"negative", -1234567890123456789},
		{"max", math.MaxInt64},
		{"min", math.MinInt64},
		{"one", 1},
		{"minus one", -1},
		{"bigint_id", 9007199254740993}, // Just over JS MAX_SAFE_INTEGER
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			hash := HashInt64(tt.value)
			// Verify hash is deterministic
			hash2 := HashInt64(tt.value)
			if hash != hash2 {
				t.Errorf("HashInt64(%d) not deterministic: %d != %d", tt.value, hash, hash2)
			}
		})
	}
}

// TestHashText tests text/varchar hashing
func TestHashText(t *testing.T) {
	tests := []struct {
		name  string
		value string
	}{
		{"empty", ""},
		{"single char", "a"},
		{"hello", "hello"},
		{"hello world", "hello world"},
		{"unicode", ""},
		{"uuid-like", "550e8400-e29b-41d4-a716-446655440000"},
		{"email", "user@example.com"},
		{"long string", "this is a much longer string that might be used as a distribution key"},
		{"special chars", "!@#$%^&*()_+-=[]{}|;':\",./<>?"},
		{"whitespace", "  spaces  "},
		{"newline", "line1\nline2"},
		{"tab", "col1\tcol2"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			hash := HashText(tt.value)
			// Verify hash is deterministic
			hash2 := HashText(tt.value)
			if hash != hash2 {
				t.Errorf("HashText(%q) not deterministic: %d != %d", tt.value, hash, hash2)
			}
		})
	}
}

// TestHashUUID tests UUID hashing
func TestHashUUID(t *testing.T) {
	tests := []struct {
		name  string
		value string
	}{
		{"nil uuid", "00000000-0000-0000-0000-000000000000"},
		{"uuid v4 example", "550e8400-e29b-41d4-a716-446655440000"},
		{"uuid v4 random", "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11"},
		{"max uuid", "ffffffff-ffff-ffff-ffff-ffffffffffff"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			u, err := uuid.Parse(tt.value)
			if err != nil {
				t.Fatalf("Failed to parse UUID %s: %v", tt.value, err)
			}
			hash := HashUUID(u)
			// Verify hash is deterministic
			hash2 := HashUUID(u)
			if hash != hash2 {
				t.Errorf("HashUUID(%s) not deterministic: %d != %d", tt.value, hash, hash2)
			}
		})
	}
}

// TestHashUUIDString tests UUID string parsing and hashing
func TestHashUUIDString(t *testing.T) {
	tests := []struct {
		name    string
		value   string
		wantErr bool
	}{
		{"valid uuid", "550e8400-e29b-41d4-a716-446655440000", false},
		{"invalid uuid", "not-a-uuid", true},
		{"empty string", "", true},
		{"partial uuid", "550e8400-e29b-41d4", true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			hash, err := HashUUIDString(tt.value)
			if (err != nil) != tt.wantErr {
				t.Errorf("HashUUIDString(%q) error = %v, wantErr %v", tt.value, err, tt.wantErr)
				return
			}
			if !tt.wantErr {
				// Verify deterministic for valid UUIDs
				hash2, _ := HashUUIDString(tt.value)
				if hash != hash2 {
					t.Errorf("HashUUIDString(%q) not deterministic: %d != %d", tt.value, hash, hash2)
				}
			}
		})
	}
}

// TestGetShardIndex tests shard index calculation
func TestGetShardIndex(t *testing.T) {
	tests := []struct {
		name       string
		hashValue  int32
		shardCount int32
		expected   int32
	}{
		{"zero hash, 4 shards", 0, 4, 0},
		{"positive hash, 4 shards", 100, 4, 0}, // 100 % 4 = 0
		{"positive hash 2, 4 shards", 101, 4, 1},
		{"positive hash 3, 4 shards", 102, 4, 2},
		{"positive hash 4, 4 shards", 103, 4, 3},
		{"negative hash, 4 shards", -1, 4, 3}, // uint32(-1) = MaxUint32, MaxUint32 % 4 = 3
		{"max int32, 4 shards", math.MaxInt32, 4, 3},
		{"min int32, 4 shards", math.MinInt32, 4, 0},
		{"zero shards", 100, 0, 0},
		{"negative shards", 100, -1, 0},
		{"one shard", 12345, 1, 0},
		{"32 shards", 100, 32, 4},
		{"64 shards", 255, 64, 63},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			result := GetShardIndex(tt.hashValue, tt.shardCount)
			if result != tt.expected {
				t.Errorf("GetShardIndex(%d, %d) = %d, want %d", tt.hashValue, tt.shardCount, result, tt.expected)
			}
		})
	}
}

// TestShardDistribution verifies that hash distribution is reasonably uniform
func TestShardDistribution(t *testing.T) {
	shardCounts := []int32{4, 8, 16, 32, 64}
	numValues := 100000

	for _, shardCount := range shardCounts {
		t.Run(fmt.Sprintf("%d_shards", shardCount), func(t *testing.T) {
			distribution := make([]int, shardCount)

			for i := 0; i < numValues; i++ {
				shard := GetShardIndexForInt64(int64(i), shardCount)
				distribution[shard]++
			}

			// Calculate expected distribution
			expected := numValues / int(shardCount)
			tolerance := float64(expected) * 0.15 // Allow 15% deviation

			for shard, count := range distribution {
				deviation := math.Abs(float64(count - expected))
				if deviation > tolerance {
					t.Errorf("Shard %d has %d values, expected ~%d (deviation %.2f%% exceeds 15%%)",
						shard, count, expected, (deviation/float64(expected))*100)
				}
			}
		})
	}
}

// TestHashValueInterface tests the generic HashValue function
func TestHashValueInterface(t *testing.T) {
	tests := []struct {
		name    string
		value   interface{}
		wantOk  bool
	}{
		{"int16", int16(100), true},
		{"int32", int32(100), true},
		{"int64", int64(100), true},
		{"int", 100, true},
		{"uint16", uint16(100), true},
		{"uint32", uint32(100), true},
		{"uint64", uint64(100), true},
		{"uint", uint(100), true},
		{"string", "test", true},
		{"bytes", []byte("test"), true},
		{"uuid", uuid.MustParse("550e8400-e29b-41d4-a716-446655440000"), true},
		{"float64", 1.0, false},
		{"nil", nil, false},
		{"struct", struct{}{}, false},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, ok := HashValue(tt.value)
			if ok != tt.wantOk {
				t.Errorf("HashValue(%v) ok = %v, want %v", tt.value, ok, tt.wantOk)
			}
		})
	}
}

// TestRouter tests the Router helper type
func TestRouter(t *testing.T) {
	router := NewRouter(8)

	if router.ShardCount() != 8 {
		t.Errorf("ShardCount() = %d, want 8", router.ShardCount())
	}

	// Test routing methods return valid shard indices
	tests := []struct {
		name   string
		route  func() int32
	}{
		{"RouteInt32", func() int32 { return router.RouteInt32(12345) }},
		{"RouteInt64", func() int32 { return router.RouteInt64(9876543210) }},
		{"RouteText", func() int32 { return router.RouteText("user@example.com") }},
		{"RouteUUID", func() int32 { return router.RouteUUID(uuid.MustParse("550e8400-e29b-41d4-a716-446655440000")) }},
		{"Route int", func() int32 { return router.Route(42) }},
		{"Route string", func() int32 { return router.Route("test") }},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			shard := tt.route()
			if shard < 0 || shard >= 8 {
				t.Errorf("%s returned invalid shard index: %d", tt.name, shard)
			}
		})
	}

	// Test unsupported type
	shard := router.Route(3.14)
	if shard != -1 {
		t.Errorf("Route(float64) = %d, want -1", shard)
	}
}

// TestDeterministicHashing verifies that the same value always produces the same hash
func TestDeterministicHashing(t *testing.T) {
	iterations := 1000

	// Test int32
	for i := 0; i < iterations; i++ {
		value := int32(i * 1000)
		hash1 := HashInt32(value)
		hash2 := HashInt32(value)
		if hash1 != hash2 {
			t.Fatalf("HashInt32(%d) not deterministic after %d iterations", value, i)
		}
	}

	// Test text
	for i := 0; i < iterations; i++ {
		value := fmt.Sprintf("test-value-%d", i)
		hash1 := HashText(value)
		hash2 := HashText(value)
		if hash1 != hash2 {
			t.Fatalf("HashText(%q) not deterministic after %d iterations", value, i)
		}
	}
}

// TestEdgeCases tests various edge cases
func TestEdgeCases(t *testing.T) {
	t.Run("empty byte slice", func(t *testing.T) {
		hash := HashBytes([]byte{})
		// Empty data should still produce a valid hash
		if hash == 0 {
			// CRC32 of empty data is 0, which is valid
		}
	})

	t.Run("nil byte slice", func(t *testing.T) {
		hash := HashBytes(nil)
		// nil should be treated same as empty
		emptyHash := HashBytes([]byte{})
		if hash != emptyHash {
			t.Errorf("HashBytes(nil) = %d, HashBytes([]) = %d, want equal", hash, emptyHash)
		}
	})

	t.Run("unicode text", func(t *testing.T) {
		testCases := []string{
			"",            // Japanese
			"",            // Chinese
			"",               // Emoji
			"Schon",             // German
			"",             // Russian
		}
		for _, s := range testCases {
			hash := HashText(s)
			hash2 := HashText(s)
			if hash != hash2 {
				t.Errorf("HashText(%q) not deterministic", s)
			}
		}
	})

	t.Run("max values", func(t *testing.T) {
		HashInt16(math.MaxInt16)
		HashInt16(math.MinInt16)
		HashInt32(math.MaxInt32)
		HashInt32(math.MinInt32)
		HashInt64(math.MaxInt64)
		HashInt64(math.MinInt64)
		// Should not panic
	})
}

// TestConsistencyWithKnownValues tests against known hash values
// These values should be verified against the actual Orochi DB implementation
func TestConsistencyWithKnownValues(t *testing.T) {
	// These test cases can be populated by running:
	// SELECT hash_value(1::int4), hash_value(100::int4), hash_value('test'::text);
	// in a PostgreSQL database with the Orochi extension installed

	t.Run("known int32 hashes", func(t *testing.T) {
		// Placeholder: These should be verified against actual Orochi output
		hash0 := HashInt32(0)
		hash1 := HashInt32(1)
		hash100 := HashInt32(100)

		// At minimum, verify they're different
		if hash0 == hash1 {
			t.Error("Hash of 0 and 1 should differ")
		}
		if hash1 == hash100 {
			t.Error("Hash of 1 and 100 should differ")
		}
	})

	t.Run("known text hashes", func(t *testing.T) {
		// Placeholder: These should be verified against actual Orochi output
		hashEmpty := HashText("")
		hashTest := HashText("test")
		hashHello := HashText("hello")

		// At minimum, verify they're different
		if hashEmpty == hashTest {
			t.Error("Hash of '' and 'test' should differ")
		}
		if hashTest == hashHello {
			t.Error("Hash of 'test' and 'hello' should differ")
		}
	})
}

// Benchmarks

func BenchmarkOrochiHash(b *testing.B) {
	data := []byte("benchmark test data for hashing")
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		OrochiHash(data)
	}
}

func BenchmarkHashInt32(b *testing.B) {
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		HashInt32(int32(i))
	}
}

func BenchmarkHashInt64(b *testing.B) {
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		HashInt64(int64(i))
	}
}

func BenchmarkHashText(b *testing.B) {
	text := "benchmark-distribution-key-value"
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		HashText(text)
	}
}

func BenchmarkHashTextLong(b *testing.B) {
	text := "this is a much longer text value that might be used as a distribution key in some applications"
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		HashText(text)
	}
}

func BenchmarkHashUUID(b *testing.B) {
	u := uuid.MustParse("550e8400-e29b-41d4-a716-446655440000")
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		HashUUID(u)
	}
}

func BenchmarkGetShardIndex(b *testing.B) {
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		GetShardIndex(int32(i), 32)
	}
}

func BenchmarkGetShardIndexForInt64(b *testing.B) {
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		GetShardIndexForInt64(int64(i), 32)
	}
}

func BenchmarkRouterRouteInt64(b *testing.B) {
	router := NewRouter(32)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		router.RouteInt64(int64(i))
	}
}

func BenchmarkRouterRouteText(b *testing.B) {
	router := NewRouter(32)
	text := "user@example.com"
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		router.RouteText(text)
	}
}
