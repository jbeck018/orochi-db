# Connection Pooler Benchmarks for Orochi DB
#
# Benchmarks connection pooler performance including:
# - Connection establishment rate
# - Pool checkout latency (p50, p95, p99)
# - Pool saturation behavior
# - Idle connection overhead
# - Transaction vs session pooling
# - TLS handshake overhead

CC ?= gcc

# Detect PostgreSQL paths using pg_config
PG_CONFIG ?= pg_config
PG_LIBDIR := $(shell $(PG_CONFIG) --libdir 2>/dev/null || echo "/opt/homebrew/lib/postgresql@14")
PG_INCLUDEDIR := $(shell $(PG_CONFIG) --includedir 2>/dev/null || echo "/opt/homebrew/include/postgresql@14")

CFLAGS ?= -std=c11 -O3 -Wall -Wextra -Wpedantic -I$(PG_INCLUDEDIR)
LDFLAGS ?= -L$(PG_LIBDIR) -lpq -lm -lpthread
COMMON_LIB ?= ../common/libbench_common.a

# Debug build flags
ifdef DEBUG
CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Wpedantic -DDEBUG
endif

# Output directories
OUTPUT_DIR ?= results
RESULTS_DIR ?= $(OUTPUT_DIR)/connection

# Database settings
PGHOST ?= localhost
PGPORT ?= 5432
PGUSER ?= postgres
PGDATABASE ?= orochi_bench

# Pooler ports
PGBOUNCER_PORT ?= 6432
PGCAT_PORT ?= 6433
PGDOG_PORT ?= 6434

# Benchmark settings
DURATION ?= 30
WARMUP ?= 5
CLIENTS ?= 10,50,100,200,500,1000

# Source files
SRCS := connection_bench.c
OBJS := $(SRCS:.c=.o)
TARGET := connection_bench

# Shell scripts
SCRIPTS := run_connection.sh pooler_compare.sh

.PHONY: all clean build run run-direct run-pgbouncer run-pgcat run-pgdog compare help setup

all: build

# Build the benchmark binary
build: $(TARGET)

$(TARGET): $(OBJS) $(COMMON_LIB)
	$(CC) $(OBJS) $(COMMON_LIB) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -I../common -c $< -o $@

# Ensure common library exists
$(COMMON_LIB):
	$(MAKE) -C ../common

# Setup schema and dependencies
setup: schema.sql
	@echo "Setting up connection benchmark..."
	@mkdir -p $(RESULTS_DIR)
	PGPASSWORD=$(PGPASSWORD) psql -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) -f schema.sql
	@chmod +x $(SCRIPTS)
	@echo "Setup complete"

# Initialize pgbench tables
init-pgbench:
	@echo "Initializing pgbench tables..."
	pgbench -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) -i -s 1

# Run benchmarks against direct PostgreSQL
run-direct: $(TARGET)
	@echo "Running benchmarks against direct PostgreSQL..."
	./run_connection.sh \
		--pooler=direct \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--duration=$(DURATION) \
		--warmup=$(WARMUP) \
		--clients=$(CLIENTS) \
		--output=$(RESULTS_DIR)

# Run benchmarks against PgBouncer
run-pgbouncer: $(TARGET)
	@echo "Running benchmarks against PgBouncer..."
	./run_connection.sh \
		--pooler=pgbouncer \
		--host=$(PGHOST) \
		--port=$(PGBOUNCER_PORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--duration=$(DURATION) \
		--warmup=$(WARMUP) \
		--clients=$(CLIENTS) \
		--output=$(RESULTS_DIR)

# Run benchmarks against PgCat
run-pgcat: $(TARGET)
	@echo "Running benchmarks against PgCat..."
	./run_connection.sh \
		--pooler=pgcat \
		--host=$(PGHOST) \
		--port=$(PGCAT_PORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--duration=$(DURATION) \
		--warmup=$(WARMUP) \
		--clients=$(CLIENTS) \
		--output=$(RESULTS_DIR)

# Run benchmarks against PgDog
run-pgdog: $(TARGET)
	@echo "Running benchmarks against PgDog..."
	./run_connection.sh \
		--pooler=pgdog \
		--host=$(PGHOST) \
		--port=$(PGDOG_PORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--duration=$(DURATION) \
		--warmup=$(WARMUP) \
		--clients=$(CLIENTS) \
		--output=$(RESULTS_DIR)

# Run all benchmarks (direct only)
run: run-direct
	@echo "Connection benchmarks complete"

# Compare all poolers
compare: $(TARGET)
	@echo "Running pooler comparison..."
	./pooler_compare.sh \
		--host=$(PGHOST) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--duration=$(DURATION) \
		--warmup=$(WARMUP) \
		--clients=$(CLIENTS) \
		--output=$(RESULTS_DIR)/comparison \
		--skip-unavailable

# Quick test with minimal settings
quick-test: $(TARGET)
	@echo "Running quick test..."
	./$(TARGET) \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--pooler=direct \
		--bench=checkout \
		--clients=10 \
		--duration=10 \
		--warmup=2 \
		--format=text

# Saturation sweep test
saturation: $(TARGET)
	@echo "Running saturation sweep..."
	./$(TARGET) \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--pooler=direct \
		--bench=saturation \
		--duration=$(DURATION) \
		--warmup=$(WARMUP) \
		--output=$(RESULTS_DIR)

# TLS overhead test
tls-test: $(TARGET)
	@echo "Running TLS overhead test..."
	./$(TARGET) \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--bench=tls \
		--output=$(RESULTS_DIR)

# Idle connection test
idle-test: $(TARGET)
	@echo "Running idle connection test..."
	./$(TARGET) \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--database=$(PGDATABASE) \
		--bench=idle \
		--idle=500 \
		--duration=30 \
		--output=$(RESULTS_DIR)

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf $(RESULTS_DIR)/*.json $(RESULTS_DIR)/*.csv

# Deep clean including results
distclean: clean
	rm -rf $(OUTPUT_DIR)

# Format code
format:
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format -i $(SRCS); \
		echo "Code formatted"; \
	else \
		echo "clang-format not found"; \
	fi

# Check formatting
check-format:
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format --dry-run -Werror $(SRCS); \
		echo "Format check passed"; \
	else \
		echo "clang-format not found"; \
	fi

# Static analysis
lint:
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all --std=c11 $(SRCS); \
	else \
		echo "cppcheck not found"; \
	fi

# Help
help:
	@echo "Connection Pooler Benchmark Suite"
	@echo ""
	@echo "Build targets:"
	@echo "  all          - Build the benchmark binary (default)"
	@echo "  build        - Build the benchmark binary"
	@echo "  clean        - Remove build artifacts"
	@echo "  distclean    - Remove all generated files"
	@echo ""
	@echo "Setup targets:"
	@echo "  setup        - Setup benchmark schema and permissions"
	@echo "  init-pgbench - Initialize pgbench tables"
	@echo ""
	@echo "Benchmark targets:"
	@echo "  run          - Run benchmarks against direct PostgreSQL"
	@echo "  run-direct   - Run benchmarks against direct PostgreSQL"
	@echo "  run-pgbouncer - Run benchmarks against PgBouncer"
	@echo "  run-pgcat    - Run benchmarks against PgCat"
	@echo "  run-pgdog    - Run benchmarks against PgDog"
	@echo "  compare      - Compare all available poolers"
	@echo "  quick-test   - Quick sanity test"
	@echo "  saturation   - Run saturation sweep test"
	@echo "  tls-test     - Run TLS overhead test"
	@echo "  idle-test    - Run idle connection test"
	@echo ""
	@echo "Code quality:"
	@echo "  format       - Format source code"
	@echo "  check-format - Check code formatting"
	@echo "  lint         - Run static analysis"
	@echo ""
	@echo "Configuration variables:"
	@echo "  PGHOST=$(PGHOST)"
	@echo "  PGPORT=$(PGPORT)"
	@echo "  PGUSER=$(PGUSER)"
	@echo "  PGDATABASE=$(PGDATABASE)"
	@echo "  DURATION=$(DURATION)"
	@echo "  WARMUP=$(WARMUP)"
	@echo "  CLIENTS=$(CLIENTS)"
	@echo "  PGBOUNCER_PORT=$(PGBOUNCER_PORT)"
	@echo "  PGCAT_PORT=$(PGCAT_PORT)"
	@echo "  PGDOG_PORT=$(PGDOG_PORT)"
	@echo ""
	@echo "Examples:"
	@echo "  make run PGHOST=db.example.com DURATION=60"
	@echo "  make run-pgbouncer PGBOUNCER_PORT=6432"
	@echo "  make compare CLIENTS=10,50,100,500"
