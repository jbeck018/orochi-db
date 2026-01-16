# Vectorized Execution Benchmarks for Orochi DB

CC ?= gcc
CFLAGS ?= -std=c11 -O3 -march=native -Wall -Wextra
LDFLAGS ?= -lpq -lm -lpthread
COMMON_LIB ?= ../common/libbench_common.a

OUTPUT_DIR ?= ../results/vectorized
PGHOST ?= localhost
PGPORT ?= 5432
PGUSER ?= postgres
PGDATABASE ?= orochi_bench

SRCS := vectorized_bench.c simd_bench.c
OBJS := $(SRCS:.c=.o)
TARGET := vectorized_bench

# Check for AVX support
AVX_SUPPORT := $(shell grep -c avx /proc/cpuinfo 2>/dev/null || echo 0)
AVX2_SUPPORT := $(shell grep -c avx2 /proc/cpuinfo 2>/dev/null || echo 0)
AVX512_SUPPORT := $(shell grep -c avx512 /proc/cpuinfo 2>/dev/null || echo 0)

ifneq ($(AVX512_SUPPORT),0)
    CFLAGS += -mavx512f -DUSE_AVX512
else ifneq ($(AVX2_SUPPORT),0)
    CFLAGS += -mavx2 -DUSE_AVX2
else ifneq ($(AVX_SUPPORT),0)
    CFLAGS += -mavx -DUSE_AVX
endif

.PHONY: all clean run setup run-simd run-batch run-memory

all: $(TARGET)

$(TARGET): $(OBJS) $(COMMON_LIB)
	$(CC) $(OBJS) $(COMMON_LIB) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -I../common -c $< -o $@

setup: schema.sql
	@echo "Creating vectorized benchmark schema..."
	PGPASSWORD=$(PGPASSWORD) psql -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) -f schema.sql

run-simd: $(TARGET)
	@echo "Running SIMD vs scalar benchmark..."
	./$(TARGET) --mode=simd --output=$(OUTPUT_DIR)

run-batch: $(TARGET)
	@echo "Running batch size optimization benchmark..."
	./$(TARGET) --mode=batch \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)

run-memory: $(TARGET)
	@echo "Running memory bandwidth benchmark..."
	./$(TARGET) --mode=memory --output=$(OUTPUT_DIR)

run: run-simd run-batch run-memory
	@echo "Vectorized benchmarks complete"

clean:
	rm -f $(OBJS) $(TARGET)
