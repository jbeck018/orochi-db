# Sharding/Distribution Benchmarks for Orochi DB
#
# Comprehensive benchmarking suite for sharding performance:
# - Distribution uniformity (RSD/Skew Score)
# - Hash vs Range comparison
# - Query routing efficiency
# - Rebalancing overhead
# - Scalability factor calculation
#

# Compiler settings
CC ?= gcc

# Detect PostgreSQL paths using pg_config
PG_CONFIG ?= pg_config
PG_LIBDIR := $(shell $(PG_CONFIG) --libdir 2>/dev/null || echo "/opt/homebrew/lib/postgresql@14")
PG_INCLUDEDIR := $(shell $(PG_CONFIG) --includedir 2>/dev/null || echo "/opt/homebrew/include/postgresql@14")

CFLAGS ?= -std=c11 -O3 -Wall -Wextra -Wno-unused-parameter -I$(PG_INCLUDEDIR)
LDFLAGS ?= -L$(PG_LIBDIR) -lpq -lm -lpthread
COMMON_LIB ?= ../common/libbench_common.a

# Database connection
PGHOST ?= localhost
PGPORT ?= 5432
PGUSER ?= postgres
PGDATABASE ?= orochi_bench

# Benchmark parameters
SCALE_FACTOR ?= 1
RECORDS ?= 10000
DURATION ?= 30
WARMUP ?= 5
SHARD_COUNTS ?= 2,4,8,16,32,64
CLIENT_COUNTS ?= 1,4,8,16,32,64

# Output
OUTPUT_DIR ?= ../results/sharding

# Source files
SRCS := sharding_bench.c
OBJS := $(SRCS:.c=.o)
TARGET := sharding_bench

# Shell scripts
SCRIPTS := run_sharding.sh distribution_test.sh rebalance_test.sh

.PHONY: all clean run setup help
.PHONY: run-distribution run-hash-range run-routing run-rebalance run-scalability
.PHONY: run-quick report

# Default target
all: $(TARGET)

# Build benchmark executable
$(TARGET): $(OBJS) $(COMMON_LIB)
	$(CC) $(OBJS) $(COMMON_LIB) $(LDFLAGS) -o $@

# Compile object files
%.o: %.c
	$(CC) $(CFLAGS) -I../common -c $< -o $@

# Ensure common library exists
$(COMMON_LIB):
	$(MAKE) -C ../common

# Setup benchmark schema
setup: schema.sql
	@echo "=== Setting up sharding benchmark schema ==="
	PGPASSWORD=$(PGPASSWORD) psql \
		-h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) \
		-f schema.sql
	@echo "=== Schema setup complete ==="
	@echo ""
	@echo "To populate data, run:"
	@echo "  psql -d $(PGDATABASE) -c \"SELECT setup_shard_benchmark($(RECORDS));\""

# Run full benchmark suite
run: $(TARGET) setup
	@echo "=== Running Sharding Benchmark Suite ==="
	@mkdir -p $(OUTPUT_DIR)
	./$(TARGET) \
		--mode=all \
		--records=$(RECORDS) \
		--shards=$(SHARD_COUNTS) \
		--clients=$(CLIENT_COUNTS) \
		--duration=$(DURATION) \
		--warmup=$(WARMUP) \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)/sharding_results.json

# Run distribution uniformity test
run-distribution: $(TARGET)
	@echo "=== Running Distribution Uniformity Test ==="
	@mkdir -p $(OUTPUT_DIR)
	./$(TARGET) \
		--mode=distribution \
		--records=$(RECORDS) \
		--shards=$(SHARD_COUNTS) \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)/distribution_results.json

# Run hash vs range comparison
run-hash-range: $(TARGET)
	@echo "=== Running Hash vs Range Comparison ==="
	@mkdir -p $(OUTPUT_DIR)
	./$(TARGET) \
		--mode=hash-vs-range \
		--records=$(RECORDS) \
		--shards=8 \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)/hash_vs_range_results.json

# Run query routing efficiency test
run-routing: $(TARGET)
	@echo "=== Running Query Routing Test ==="
	@mkdir -p $(OUTPUT_DIR)
	./$(TARGET) \
		--mode=routing \
		--records=$(RECORDS) \
		--shards=8 \
		--clients=$(CLIENT_COUNTS) \
		--duration=$(DURATION) \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)/routing_results.json

# Run rebalancing overhead test
run-rebalance: $(TARGET)
	@echo "=== Running Rebalancing Overhead Test ==="
	@mkdir -p $(OUTPUT_DIR)
	./$(TARGET) \
		--mode=rebalance \
		--records=$(RECORDS) \
		--shards=8 \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)/rebalance_results.json

# Run scalability factor test
run-scalability: $(TARGET)
	@echo "=== Running Scalability Test ==="
	@mkdir -p $(OUTPUT_DIR)
	./$(TARGET) \
		--mode=scalability \
		--records=$(RECORDS) \
		--shards=$(SHARD_COUNTS) \
		--clients=$(CLIENT_COUNTS) \
		--duration=$(DURATION) \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)/scalability_results.json

# Quick test with reduced configurations
run-quick: $(TARGET)
	@echo "=== Running Quick Sharding Test ==="
	@mkdir -p $(OUTPUT_DIR)
	./$(TARGET) \
		--mode=all \
		--records=1000 \
		--shards=2,4,8 \
		--clients=1,4 \
		--duration=10 \
		--warmup=2 \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)/quick_results.json

# Run shell-based distribution test
run-distribution-shell:
	@echo "=== Running Shell Distribution Test ==="
	./distribution_test.sh all

# Run shell-based rebalance test
run-rebalance-shell:
	@echo "=== Running Shell Rebalance Test ==="
	./rebalance_test.sh run

# Generate report from results
report:
	@echo "=== Generating Benchmark Report ==="
	@./run_sharding.sh report

# Make scripts executable
scripts:
	chmod +x $(SCRIPTS)

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)

# Clean results
clean-results:
	rm -rf $(OUTPUT_DIR)

# Clean everything
clean-all: clean clean-results

# Help
help:
	@echo "Sharding/Distribution Benchmarks for Orochi DB"
	@echo ""
	@echo "Usage: make [target] [options]"
	@echo ""
	@echo "Targets:"
	@echo "  all              Build benchmark executable (default)"
	@echo "  setup            Create benchmark schema in database"
	@echo "  run              Run full benchmark suite"
	@echo "  run-distribution Run distribution uniformity test"
	@echo "  run-hash-range   Run hash vs range comparison"
	@echo "  run-routing      Run query routing efficiency test"
	@echo "  run-rebalance    Run rebalancing overhead test"
	@echo "  run-scalability  Run scalability factor test"
	@echo "  run-quick        Run quick test (reduced configs)"
	@echo "  report           Generate report from results"
	@echo "  clean            Remove build artifacts"
	@echo "  clean-results    Remove benchmark results"
	@echo "  clean-all        Remove everything"
	@echo "  help             Show this help"
	@echo ""
	@echo "Options:"
	@echo "  RECORDS=N        Number of base records [default: 10000]"
	@echo "  DURATION=N       Test duration in seconds [default: 30]"
	@echo "  WARMUP=N         Warmup duration in seconds [default: 5]"
	@echo "  SHARD_COUNTS=N   Comma-separated shard counts [default: 2,4,8,16,32,64]"
	@echo "  CLIENT_COUNTS=N  Comma-separated client counts [default: 1,4,8,16,32,64]"
	@echo "  PGHOST=HOST      Database host [default: localhost]"
	@echo "  PGPORT=PORT      Database port [default: 5432]"
	@echo "  PGUSER=USER      Database user [default: postgres]"
	@echo "  PGDATABASE=DB    Database name [default: orochi_bench]"
	@echo "  OUTPUT_DIR=DIR   Output directory [default: ../results/sharding]"
	@echo ""
	@echo "Metrics:"
	@echo "  - RSD (Skew Score): Target < 10% for acceptable distribution"
	@echo "  - Scalability Factor: Target > 0.7 (70% linear scaling)"
	@echo "  - Rebalance Performance Drop: Target < 50%"
	@echo ""
	@echo "Examples:"
	@echo "  make run RECORDS=50000 DURATION=60"
	@echo "  make run-scalability SHARD_COUNTS=4,8,16 CLIENT_COUNTS=1,4,8"
	@echo "  make run-distribution"
	@echo "  make run-quick"
