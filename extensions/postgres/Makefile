# Orochi DB - Modern HTAP PostgreSQL Extension
# Makefile for building the extension

EXTENSION = orochi
EXTVERSION = 1.0

MODULE_big = orochi
OBJS = \
	src/core/init.o \
	src/core/catalog.o \
	src/storage/columnar.o \
	src/storage/compression.o \
	src/sharding/distribution.o \
	src/sharding/physical_sharding.o \
	src/timeseries/hypertable.o \
	src/tiered/tiered_storage.o \
	src/tiered/s3_multipart.o \
	src/vector/vector_ops.o \
	src/planner/distributed_planner.o \
	src/executor/distributed_executor.o \
	src/executor/vectorized.o \
	src/executor/vectorized_scan.o \
	src/jit/jit_compile.o \
	src/jit/jit_expr.o \
	src/consensus/raft.o \
	src/consensus/raft_log.o \
	src/consensus/raft_stubs.o \
	src/approx/hyperloglog.o \
	src/approx/tdigest.o \
	src/approx/sampling.o \
	src/pipelines/pipeline.o \
	src/pipelines/kafka_source.o \
	src/json/json_index.o \
	src/json/json_columnar.o \
	src/json/json_query.o \
	src/ddl/ddl_workflow.o \
	src/ddl/ddl_stream.o \
	src/ddl/ddl_task.o \
	src/ddl/ddl_dynamic.o \
	src/utils/utils.o

DATA = sql/orochi--1.0.sql sql/ddl.sql
PGFILEDESC = "Orochi DB - Modern HTAP PostgreSQL Extension"

# Compiler flags
# Include both src directory and libpq headers (for raft consensus using libpq)
PG_CPPFLAGS = -I$(srcdir)/src -I$(shell $(PG_CONFIG) --includedir)
PG_CFLAGS = -std=c11

# Enable SIMD optimizations if available
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
    PG_CFLAGS += -mavx2 -mfma
endif
ifeq ($(UNAME_M),aarch64)
    PG_CFLAGS += -march=armv8-a+simd
endif

# Debug mode
ifdef DEBUG
    PG_CFLAGS += -g -O0 -DOROCHI_DEBUG
else
    PG_CFLAGS += -O3
endif

# Warnings
PG_CFLAGS += -Wall -Wextra -Wno-unused-parameter

# Link against compression libraries, OpenSSL (for S3 signing), Kafka, and libpq (for distributed queries)
SHLIB_LINK = -llz4 -lzstd -lcurl -lssl -lcrypto -lrdkafka -lpq

# Regression tests
REGRESS = basic distributed hypertable columnar vector tiering json

# Documentation
DOCS = docs/architecture.md docs/user-guide.md

# Use PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PostgreSQL version requirements
PG_VERSION := $(shell $(PG_CONFIG) --version | sed 's/PostgreSQL //' | cut -d. -f1)
MIN_PG_VERSION := 16
MAX_PG_VERSION := 18

# Additional targets
.PHONY: format check-format lint test docs clean-all check-version

# Check PostgreSQL version compatibility
check-version:
	@echo "Checking PostgreSQL version..."
	@if [ $(PG_VERSION) -lt $(MIN_PG_VERSION) ] || [ $(PG_VERSION) -gt $(MAX_PG_VERSION) ]; then \
		echo "Error: Orochi DB requires PostgreSQL $(MIN_PG_VERSION)-$(MAX_PG_VERSION), found $(PG_VERSION)"; \
		exit 1; \
	fi
	@echo "PostgreSQL $(PG_VERSION) supported"

# Format source code
format:
	find src -name '*.c' -o -name '*.h' | xargs clang-format -i

# Check formatting
check-format:
	find src -name '*.c' -o -name '*.h' | xargs clang-format --dry-run --Werror

# Run static analysis
lint:
	cppcheck --enable=all --suppress=missingIncludeSystem src/

# Run tests
test: install
	$(MAKE) installcheck

# Generate documentation
docs:
	@echo "Documentation is in docs/ directory"

# Clean everything including generated files
clean-all: clean
	rm -rf results/ regression.diffs regression.out

# Install development dependencies (Ubuntu/Debian)
# Supports PostgreSQL 16, 17, and 18
install-deps:
	sudo apt-get install -y \
		postgresql-server-dev-all \
		liblz4-dev \
		libzstd-dev \
		libcurl4-openssl-dev \
		libssl-dev \
		librdkafka-dev \
		clang-format \
		cppcheck

# Create release tarball
dist:
	git archive --format=tar.gz --prefix=orochi-$(EXTVERSION)/ \
		-o orochi-$(EXTVERSION).tar.gz HEAD
