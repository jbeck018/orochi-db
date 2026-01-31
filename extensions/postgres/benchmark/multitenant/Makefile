# Multi-Tenant Isolation Benchmarks for Orochi DB
#
# This benchmark suite measures multi-tenant isolation performance:
# - Baseline per-tenant performance
# - Concurrent multi-tenant workload
# - Resource fairness (Jain's Fairness Index)
# - Noisy neighbor detection
# - SLA compliance rates

CC ?= gcc
CFLAGS ?= -std=c11 -O3 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -lpq -lm -lpthread
COMMON_LIB ?= ../common/libbench_common.a

# PostgreSQL configuration
PGCONFIG ?= pg_config
PG_INCLUDE := $(shell $(PGCONFIG) --includedir-server 2>/dev/null)
PG_LIBDIR := $(shell $(PGCONFIG) --libdir 2>/dev/null)

# Add PostgreSQL include path if available
ifdef PG_INCLUDE
    CFLAGS += -I$(PG_INCLUDE)
endif

ifdef PG_LIBDIR
    LDFLAGS += -L$(PG_LIBDIR)
endif

# Output directory
OUTPUT_DIR ?= ../results/multitenant

# Database settings
PGHOST ?= localhost
PGPORT ?= 5432
PGUSER ?= postgres
PGDATABASE ?= orochi_bench

# Benchmark parameters
NUM_TENANTS ?= 10
ROWS_PER_TENANT ?= 10000
DURATION ?= 60
WARMUP ?= 10
SLA_LATENCY ?= 100
ISOLATION ?= shared_table
WORKLOAD ?= mixed

# Source files
SRCS := tenant_bench.c
OBJS := $(SRCS:.c=.o)
TARGET := tenant_bench

# Shell scripts
SCRIPTS := run_multitenant.sh isolation_test.sh

.PHONY: all clean run setup teardown help
.PHONY: run-quick run-stress run-all-workloads run-all-isolations
.PHONY: test-isolation install format

# Default target
all: $(TARGET)

# Build benchmark executable
$(TARGET): $(OBJS) $(COMMON_LIB)
	$(CC) $(OBJS) $(COMMON_LIB) $(LDFLAGS) -o $@
	@echo "Built $(TARGET) successfully"

# Compile object files
%.o: %.c
	$(CC) $(CFLAGS) -I../common -c $< -o $@

# Ensure common library exists
$(COMMON_LIB):
	$(MAKE) -C ../common

# Database setup
setup: schema.sql
	@echo "=== Setting up multi-tenant schema ==="
	@mkdir -p $(OUTPUT_DIR)
	PGPASSWORD=$(PGPASSWORD) psql -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) -f schema.sql
	@echo "=== Schema setup complete ==="

# Database teardown
teardown:
	@echo "=== Tearing down multi-tenant schema ==="
	PGPASSWORD=$(PGPASSWORD) psql -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) -c "\
		DROP TABLE IF EXISTS tenant_data CASCADE; \
		DROP TABLE IF EXISTS tenant_metrics CASCADE; \
		DROP TABLE IF EXISTS tenant_audit_log CASCADE; \
		DROP TABLE IF EXISTS tenants CASCADE;"
	@echo "=== Teardown complete ==="

# Run benchmark with default settings
run: $(TARGET) setup
	@echo "=== Running Multi-Tenant Benchmark ==="
	./$(TARGET) \
		-h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) \
		-n $(NUM_TENANTS) -r $(ROWS_PER_TENANT) \
		-D $(DURATION) -w $(WARMUP) \
		-s $(SLA_LATENCY) \
		-i $(ISOLATION) -W $(WORKLOAD) \
		-o $(OUTPUT_DIR)/multitenant_$(ISOLATION)_$(WORKLOAD).json \
		-f json

# Quick test (5 tenants, 30 second duration)
run-quick: $(TARGET) setup
	@echo "=== Running Quick Multi-Tenant Test ==="
	./$(TARGET) \
		-h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) \
		-n 5 -r 1000 \
		-D 30 -w 5 \
		-s $(SLA_LATENCY) \
		-i $(ISOLATION) -W $(WORKLOAD) \
		-f text

# Stress test (50 tenants, 5 minute duration)
run-stress: $(TARGET) setup
	@echo "=== Running Stress Multi-Tenant Test ==="
	./$(TARGET) \
		-h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) \
		-n 50 -r 50000 \
		-D 300 -w 30 \
		-s $(SLA_LATENCY) \
		-i $(ISOLATION) -W $(WORKLOAD) \
		-o $(OUTPUT_DIR)/multitenant_stress_$(ISOLATION)_$(WORKLOAD).json \
		-f json

# Run all workload types
run-all-workloads: $(TARGET) setup
	@echo "=== Running All Workload Types ==="
	@for workload in read write mixed analytical; do \
		echo ">>> Running $$workload workload..."; \
		./$(TARGET) \
			-h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) \
			-n $(NUM_TENANTS) -r $(ROWS_PER_TENANT) \
			-D $(DURATION) -w $(WARMUP) \
			-s $(SLA_LATENCY) \
			-i $(ISOLATION) -W $$workload \
			-o $(OUTPUT_DIR)/multitenant_$(ISOLATION)_$${workload}.json \
			-f json; \
	done

# Run all isolation models
run-all-isolations: $(TARGET) setup
	@echo "=== Running All Isolation Models ==="
	@for isolation in shared_table shared_schema; do \
		echo ">>> Running $$isolation isolation..."; \
		./$(TARGET) \
			-h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) \
			-n $(NUM_TENANTS) -r $(ROWS_PER_TENANT) \
			-D $(DURATION) -w $(WARMUP) \
			-s $(SLA_LATENCY) \
			-i $$isolation -W $(WORKLOAD) \
			-o $(OUTPUT_DIR)/multitenant_$${isolation}_$(WORKLOAD).json \
			-f json; \
	done

# Run isolation verification tests
test-isolation: setup
	@echo "=== Running Isolation Verification Tests ==="
	chmod +x isolation_test.sh
	./isolation_test.sh -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) --setup

# Use shell script runner
run-shell: setup
	@echo "=== Running via Shell Script ==="
	chmod +x run_multitenant.sh
	./run_multitenant.sh \
		-h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) \
		-n $(NUM_TENANTS) -r $(ROWS_PER_TENANT) \
		-D $(DURATION) -w $(WARMUP) \
		-s $(SLA_LATENCY) \
		-i $(ISOLATION) -W $(WORKLOAD) \
		-o $(OUTPUT_DIR) -f $(OUTPUT_FORMAT)

# Install scripts to path
install: $(TARGET) $(SCRIPTS)
	@echo "Installing to /usr/local/bin..."
	install -m 755 $(TARGET) /usr/local/bin/
	install -m 755 $(SCRIPTS) /usr/local/bin/

# Format source code
format:
	@if command -v clang-format > /dev/null; then \
		clang-format -i *.c; \
		echo "Formatted source files"; \
	else \
		echo "clang-format not found"; \
	fi

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Cleaned build artifacts"

# Clean results
clean-results:
	rm -rf $(OUTPUT_DIR)/*
	@echo "Cleaned results directory"

# Clean everything
clean-all: clean clean-results teardown

# Help target
help:
	@echo "Multi-Tenant Isolation Benchmarks for Orochi DB"
	@echo ""
	@echo "Usage: make [target] [options]"
	@echo ""
	@echo "Build Targets:"
	@echo "  all              Build the benchmark executable"
	@echo "  clean            Remove build artifacts"
	@echo "  format           Format source code with clang-format"
	@echo ""
	@echo "Database Targets:"
	@echo "  setup            Create multi-tenant schema in database"
	@echo "  teardown         Remove multi-tenant schema from database"
	@echo ""
	@echo "Benchmark Targets:"
	@echo "  run              Run benchmark with default settings"
	@echo "  run-quick        Quick test (5 tenants, 30s)"
	@echo "  run-stress       Stress test (50 tenants, 300s)"
	@echo "  run-all-workloads  Run all workload types"
	@echo "  run-all-isolations Run all isolation models"
	@echo "  run-shell        Run via shell script"
	@echo ""
	@echo "Test Targets:"
	@echo "  test-isolation   Run isolation verification tests"
	@echo ""
	@echo "Options:"
	@echo "  NUM_TENANTS=N      Number of tenants [$(NUM_TENANTS)]"
	@echo "  ROWS_PER_TENANT=N  Rows per tenant [$(ROWS_PER_TENANT)]"
	@echo "  DURATION=N         Benchmark duration in seconds [$(DURATION)]"
	@echo "  WARMUP=N           Warmup duration in seconds [$(WARMUP)]"
	@echo "  SLA_LATENCY=N      SLA latency threshold in ms [$(SLA_LATENCY)]"
	@echo "  ISOLATION=X        Isolation model: shared_table, shared_schema [$(ISOLATION)]"
	@echo "  WORKLOAD=X         Workload: read, write, mixed, analytical [$(WORKLOAD)]"
	@echo "  OUTPUT_DIR=X       Output directory [$(OUTPUT_DIR)]"
	@echo "  PGHOST=X           PostgreSQL host [$(PGHOST)]"
	@echo "  PGPORT=N           PostgreSQL port [$(PGPORT)]"
	@echo "  PGUSER=X           PostgreSQL user [$(PGUSER)]"
	@echo "  PGDATABASE=X       Database name [$(PGDATABASE)]"
	@echo ""
	@echo "Examples:"
	@echo "  make run NUM_TENANTS=20 DURATION=120"
	@echo "  make run-stress ISOLATION=shared_schema"
	@echo "  make run-all-workloads WORKLOAD=mixed"
	@echo "  make test-isolation"
