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
	src/vector/vector_ops.o \
	src/planner/distributed_planner.o \
	src/executor/distributed_executor.o \
	src/utils/utils.o

DATA = sql/orochi--1.0.sql
PGFILEDESC = "Orochi DB - Modern HTAP PostgreSQL Extension"

# Compiler flags
PG_CPPFLAGS = -I$(srcdir)/src
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

# Link against compression libraries and OpenSSL (for S3 signing)
SHLIB_LINK = -llz4 -lzstd -lcurl -lssl -lcrypto

# Regression tests
REGRESS = basic distributed hypertable columnar vector tiering

# Documentation
DOCS = README.md docs/architecture.md docs/user-guide.md

# Use PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Additional targets
.PHONY: format check-format lint test docs clean-all

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
install-deps:
	sudo apt-get install -y \
		postgresql-server-dev-all \
		liblz4-dev \
		libzstd-dev \
		libcurl4-openssl-dev \
		clang-format \
		cppcheck

# Create release tarball
dist:
	git archive --format=tar.gz --prefix=orochi-$(EXTVERSION)/ \
		-o orochi-$(EXTVERSION).tar.gz HEAD
