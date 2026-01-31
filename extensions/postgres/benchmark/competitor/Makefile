# Competitor Comparison Benchmarks for Orochi DB
#
# Compares Orochi DB against:
# - TimescaleDB (time-series)
# - Citus (distributed)
# - Hydra (columnar/analytical)
# - ClickHouse (analytical)
# - Standard PostgreSQL (baseline)
#
# Using industry-standard benchmark suites:
# - TPC-C (OLTP)
# - TPC-H (analytical)
# - TSBS (time-series)
# - ClickBench (analytical)

PYTHON ?= python3
SHELL_SCRIPTS := run_comparison.sh clickbench_wrapper.sh tpcc_wrapper.sh tsbs_wrapper.sh

# Output directories
OUTPUT_DIR ?= results
CHARTS_DIR ?= charts

# Database settings
PGHOST ?= localhost
PGPORT ?= 5432
PGUSER ?= postgres

# Benchmark settings
SCALE_FACTOR ?= 1
DURATION ?= 60
WARMUP ?= 10

.PHONY: all build clean run run-tpcc run-tsbs run-clickbench compare setup help venv

# Default target - just verify scripts are executable
all: build

build:
	@echo "Making scripts executable..."
	@chmod +x $(SHELL_SCRIPTS)
	@echo "Verifying Python dependencies..."
	@$(PYTHON) -c "import yaml, matplotlib, numpy" 2>/dev/null || \
		(echo "Warning: Some Python dependencies missing. Run 'make venv' to setup virtual environment."; exit 0)
	@echo "Build complete"

# Setup Python virtual environment
venv:
	@echo "Setting up Python virtual environment..."
	$(PYTHON) -m venv .venv
	.venv/bin/pip install -r requirements.txt
	@echo "Virtual environment ready. Activate with: source .venv/bin/activate"

# Setup benchmark environment
setup:
	@echo "Setting up competitor comparison environment..."
	@mkdir -p $(OUTPUT_DIR) $(CHARTS_DIR)
	@chmod +x $(SHELL_SCRIPTS)
	@echo "Setup complete"

# Run full competitor comparison
run: setup
	@echo "Running full competitor comparison..."
	./run_comparison.sh \
		--scale=$(SCALE_FACTOR) \
		--duration=$(DURATION) \
		--output=$(OUTPUT_DIR)

# Run TPC-C comparison
run-tpcc: setup
	@echo "Running TPC-C comparison..."
	./tpcc_wrapper.sh \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--scale=$(SCALE_FACTOR) \
		--duration=$(DURATION) \
		--warmup=$(WARMUP) \
		--output=$(OUTPUT_DIR)/tpcc

# Run TSBS comparison
run-tsbs: setup
	@echo "Running TSBS comparison..."
	./tsbs_wrapper.sh \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--scale=$(SCALE_FACTOR) \
		--duration=$(DURATION) \
		--output=$(OUTPUT_DIR)/tsbs

# Run ClickBench comparison
run-clickbench: setup
	@echo "Running ClickBench comparison..."
	./clickbench_wrapper.sh \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--output=$(OUTPUT_DIR)/clickbench

# Run Python comparison orchestrator
compare: setup
	@echo "Running comprehensive comparison..."
	$(PYTHON) competitor_bench.py \
		--host=$(PGHOST) \
		--port=$(PGPORT) \
		--user=$(PGUSER) \
		--scale=$(SCALE_FACTOR) \
		--duration=$(DURATION) \
		--output=$(OUTPUT_DIR)

# Generate comparison charts
charts:
	@echo "Generating comparison charts..."
	$(PYTHON) generate_charts.py \
		--input=$(OUTPUT_DIR) \
		--output=$(CHARTS_DIR)

# Clean results
clean:
	rm -rf $(OUTPUT_DIR)/*.json $(OUTPUT_DIR)/*.csv
	rm -rf $(CHARTS_DIR)/*.png $(CHARTS_DIR)/*.svg
	rm -rf __pycache__ .pytest_cache

# Deep clean including virtual environment
distclean: clean
	rm -rf .venv $(OUTPUT_DIR) $(CHARTS_DIR)

# Docker-based comparison (spins up competitor databases)
docker-compare:
	@echo "Running Docker-based competitor comparison..."
	docker compose -f docker-compose.benchmark.yml up -d
	@sleep 30  # Wait for databases to start
	$(MAKE) compare
	docker compose -f docker-compose.benchmark.yml down

# Help
help:
	@echo "Competitor Comparison Benchmark Suite"
	@echo ""
	@echo "Build targets:"
	@echo "  all          - Verify scripts and dependencies (default)"
	@echo "  build        - Make scripts executable"
	@echo "  venv         - Setup Python virtual environment"
	@echo "  setup        - Create output directories"
	@echo "  clean        - Remove results and cache files"
	@echo "  distclean    - Remove all generated files"
	@echo ""
	@echo "Benchmark targets:"
	@echo "  run          - Run full comparison suite"
	@echo "  run-tpcc     - Run TPC-C comparison"
	@echo "  run-tsbs     - Run TSBS time-series comparison"
	@echo "  run-clickbench - Run ClickBench analytical comparison"
	@echo "  compare      - Run Python comparison orchestrator"
	@echo "  charts       - Generate comparison charts"
	@echo "  docker-compare - Run comparison using Docker containers"
	@echo ""
	@echo "Configuration variables:"
	@echo "  PGHOST=$(PGHOST)"
	@echo "  PGPORT=$(PGPORT)"
	@echo "  PGUSER=$(PGUSER)"
	@echo "  SCALE_FACTOR=$(SCALE_FACTOR)"
	@echo "  DURATION=$(DURATION)"
	@echo "  WARMUP=$(WARMUP)"
	@echo "  OUTPUT_DIR=$(OUTPUT_DIR)"
	@echo ""
	@echo "Examples:"
	@echo "  make run SCALE_FACTOR=10 DURATION=300"
	@echo "  make run-tpcc PGHOST=db.example.com"
	@echo "  make docker-compare SCALE_FACTOR=1"
