# Orochi DB Benchmarking Framework
# Makefile for building and running all benchmarks

# Configuration
PGCONFIG ?= pg_config
PG_INCLUDE := $(shell $(PGCONFIG) --includedir-server)
PG_LIBDIR := $(shell $(PGCONFIG) --libdir)
PGBIN := $(shell $(PGCONFIG) --bindir)
PSQL := $(PGBIN)/psql

# Database connection
PGHOST ?= localhost
PGPORT ?= 5432
PGUSER ?= postgres
PGDATABASE ?= orochi_bench

# Benchmark configuration
SCALE_FACTOR ?= 1
NUM_CLIENTS ?= 1
NUM_THREADS ?= 1
DURATION ?= 60
WARMUP_TIME ?= 10
OUTPUT_FORMAT ?= json
RESULTS_DIR := results

# Compiler settings
CC := gcc
CFLAGS := -std=c11 -O3 -march=native -Wall -Wextra
CFLAGS += -I$(PG_INCLUDE) -I../src -I./common
LDFLAGS := -L$(PG_LIBDIR) -lpq -lm -lpthread

# Debug build
ifdef DEBUG
    CFLAGS := -std=c11 -O0 -g -Wall -Wextra -DDEBUG
    CFLAGS += -I$(PG_INCLUDE) -I../src -I./common
endif

# Subdirectories
# NOTE: vectorized is excluded until source files are implemented
SUBDIRS := tpch timeseries columnar distributed multitenant sharding connection competitor

# Build artifacts
COMMON_SRCS := $(wildcard common/*.c)
COMMON_OBJS := $(COMMON_SRCS:.c=.o)
COMMON_LIB := common/libbench_common.a

.PHONY: all clean clean-results setup teardown help
.PHONY: $(SUBDIRS) run-all run-tpch run-timeseries run-columnar run-distributed run-vectorized run-multitenant run-sharding
.PHONY: report compare dashboard dashboard-setup dashboard-serve

# Default target
all: $(COMMON_LIB) $(SUBDIRS)

# Build common library
$(COMMON_LIB): $(COMMON_OBJS)
	ar rcs $@ $^

common/%.o: common/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Build each benchmark suite
$(SUBDIRS): $(COMMON_LIB)
	$(MAKE) -C $@ CC="$(CC)" CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" COMMON_LIB="../$(COMMON_LIB)"

# Database setup
setup:
	@echo "=== Setting up benchmark database ==="
	$(PSQL) -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -c "DROP DATABASE IF EXISTS $(PGDATABASE);"
	$(PSQL) -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -c "CREATE DATABASE $(PGDATABASE);"
	$(PSQL) -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) -c "CREATE EXTENSION IF NOT EXISTS orochi;"
	@echo "=== Database setup complete ==="

teardown:
	@echo "=== Tearing down benchmark database ==="
	$(PSQL) -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -c "DROP DATABASE IF EXISTS $(PGDATABASE);"
	@echo "=== Database teardown complete ==="

# Run all benchmarks
run-all: setup
	@echo "=== Running All Orochi DB Benchmarks ==="
	@echo "Scale Factor: $(SCALE_FACTOR)"
	@echo "Clients: $(NUM_CLIENTS)"
	@echo "Duration: $(DURATION)s"
	@echo ""
	@mkdir -p $(RESULTS_DIR)
	@./run_all.sh \
		--scale=$(SCALE_FACTOR) \
		--clients=$(NUM_CLIENTS) \
		--threads=$(NUM_THREADS) \
		--duration=$(DURATION) \
		--warmup=$(WARMUP_TIME) \
		--format=$(OUTPUT_FORMAT) \
		--output=$(RESULTS_DIR)

# Individual benchmark suites
run-tpch: setup
	@echo "=== Running TPC-H Style Benchmarks ==="
	@mkdir -p $(RESULTS_DIR)/tpch
	$(MAKE) -C tpch run \
		SCALE_FACTOR=$(SCALE_FACTOR) \
		PGHOST=$(PGHOST) PGPORT=$(PGPORT) \
		PGUSER=$(PGUSER) PGDATABASE=$(PGDATABASE) \
		OUTPUT_DIR=../$(RESULTS_DIR)/tpch

run-timeseries: setup
	@echo "=== Running Time-Series Benchmarks ==="
	@mkdir -p $(RESULTS_DIR)/timeseries
	$(MAKE) -C timeseries run \
		SCALE_FACTOR=$(SCALE_FACTOR) \
		PGHOST=$(PGHOST) PGPORT=$(PGPORT) \
		PGUSER=$(PGUSER) PGDATABASE=$(PGDATABASE) \
		OUTPUT_DIR=../$(RESULTS_DIR)/timeseries

run-columnar: setup
	@echo "=== Running Columnar Storage Benchmarks ==="
	@mkdir -p $(RESULTS_DIR)/columnar
	$(MAKE) -C columnar run \
		SCALE_FACTOR=$(SCALE_FACTOR) \
		PGHOST=$(PGHOST) PGPORT=$(PGPORT) \
		PGUSER=$(PGUSER) PGDATABASE=$(PGDATABASE) \
		OUTPUT_DIR=../$(RESULTS_DIR)/columnar

run-distributed: setup
	@echo "=== Running Distributed Benchmarks ==="
	@mkdir -p $(RESULTS_DIR)/distributed
	$(MAKE) -C distributed run \
		SCALE_FACTOR=$(SCALE_FACTOR) \
		NUM_SHARDS=$(NUM_SHARDS) \
		PGHOST=$(PGHOST) PGPORT=$(PGPORT) \
		PGUSER=$(PGUSER) PGDATABASE=$(PGDATABASE) \
		OUTPUT_DIR=../$(RESULTS_DIR)/distributed

run-vectorized: setup
	@echo "=== Running Vectorized Execution Benchmarks ==="
	@mkdir -p $(RESULTS_DIR)/vectorized
	$(MAKE) -C vectorized run \
		PGHOST=$(PGHOST) PGPORT=$(PGPORT) \
		PGUSER=$(PGUSER) PGDATABASE=$(PGDATABASE) \
		OUTPUT_DIR=../$(RESULTS_DIR)/vectorized

run-multitenant: setup
	@echo "=== Running Multi-Tenant Isolation Benchmarks ==="
	@mkdir -p $(RESULTS_DIR)/multitenant
	$(MAKE) -C multitenant run \
		NUM_TENANTS=$(NUM_TENANTS) \
		ROWS_PER_TENANT=$(ROWS_PER_TENANT) \
		DURATION=$(DURATION) \
		PGHOST=$(PGHOST) PGPORT=$(PGPORT) \
		PGUSER=$(PGUSER) PGDATABASE=$(PGDATABASE) \
		OUTPUT_DIR=../$(RESULTS_DIR)/multitenant

run-sharding: setup
	@echo "=== Running Sharding/Distribution Benchmarks ==="
	@mkdir -p $(RESULTS_DIR)/sharding
	$(MAKE) -C sharding run \
		RECORDS=$(SCALE_FACTOR)0000 \
		DURATION=$(DURATION) \
		PGHOST=$(PGHOST) PGPORT=$(PGPORT) \
		PGUSER=$(PGUSER) PGDATABASE=$(PGDATABASE) \
		OUTPUT_DIR=../$(RESULTS_DIR)/sharding

# Generate report from results
report:
	@echo "=== Generating Benchmark Report ==="
	@./common/generate_report.sh $(RESULTS_DIR) $(OUTPUT_FORMAT)

# Compare with baseline
compare:
	@echo "=== Comparing Results with Baseline ==="
	@./common/compare_results.sh $(RESULTS_DIR) $(BASELINE_DIR)

# Dashboard targets
dashboard-setup:
	@echo "=== Setting up Dashboard Dependencies ==="
	pip3 install -r dashboard/requirements.txt

dashboard: $(RESULTS_DIR)
	@echo "=== Generating Dashboard Report ==="
	cd dashboard && ./generate_report.sh -r ../$(RESULTS_DIR) -o ./output -c

dashboard-serve:
	@echo "=== Starting Dashboard Web Server ==="
	cd dashboard && python3 web_server.py -r ../$(RESULTS_DIR) -p 5000

dashboard-static:
	@echo "=== Starting Static Dashboard Server ==="
	cd dashboard/output && python3 -m http.server 8000

# Clean build artifacts
clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -f $(COMMON_OBJS) $(COMMON_LIB)

# Clean results
clean-results:
	rm -rf $(RESULTS_DIR)/*

# Clean everything
clean-all: clean clean-results

# Help target
help:
	@echo "Orochi DB Benchmarking Framework"
	@echo ""
	@echo "Usage: make [target] [options]"
	@echo ""
	@echo "Targets:"
	@echo "  all              Build all benchmark executables"
	@echo "  setup            Create and configure benchmark database"
	@echo "  teardown         Drop benchmark database"
	@echo "  run-all          Run all benchmark suites"
	@echo "  run-tpch         Run TPC-H style benchmarks"
	@echo "  run-timeseries   Run time-series benchmarks"
	@echo "  run-columnar     Run columnar storage benchmarks"
	@echo "  run-distributed  Run distributed benchmarks"
	@echo "  run-vectorized   Run vectorized execution benchmarks"
	@echo "  run-multitenant  Run multi-tenant isolation benchmarks"
	@echo "  report           Generate report from results"
	@echo "  compare          Compare results with baseline"
	@echo "  dashboard-setup  Install dashboard Python dependencies"
	@echo "  dashboard        Generate dashboard with charts"
	@echo "  dashboard-serve  Start interactive web dashboard"
	@echo "  dashboard-static Serve static dashboard files"
	@echo "  clean            Clean build artifacts"
	@echo "  clean-results    Clean benchmark results"
	@echo "  clean-all        Clean everything"
	@echo ""
	@echo "Options:"
	@echo "  SCALE_FACTOR=N   Data scale factor (1=1GB, 10=10GB, 100=100GB) [default: 1]"
	@echo "  NUM_CLIENTS=N    Number of concurrent clients [default: 1]"
	@echo "  NUM_THREADS=N    Number of threads per client [default: 1]"
	@echo "  DURATION=N       Benchmark duration in seconds [default: 60]"
	@echo "  WARMUP_TIME=N    Warmup period in seconds [default: 10]"
	@echo "  OUTPUT_FORMAT=X  Output format: json, csv [default: json]"
	@echo "  PGHOST=X         PostgreSQL host [default: localhost]"
	@echo "  PGPORT=N         PostgreSQL port [default: 5432]"
	@echo "  PGUSER=X         PostgreSQL user [default: postgres]"
	@echo "  PGDATABASE=X     Benchmark database name [default: orochi_bench]"
	@echo "  DEBUG=1          Enable debug build"
	@echo ""
	@echo "Examples:"
	@echo "  make run-all SCALE_FACTOR=10 NUM_CLIENTS=8"
	@echo "  make run-tpch SCALE_FACTOR=100 DURATION=300"
	@echo "  make run-timeseries NUM_CLIENTS=16 NUM_THREADS=4"
