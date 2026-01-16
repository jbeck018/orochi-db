# Columnar Storage Benchmarks for Orochi DB

CC ?= gcc
CFLAGS ?= -std=c11 -O3 -Wall -Wextra
LDFLAGS ?= -lpq -lm -lpthread
COMMON_LIB ?= ../common/libbench_common.a

OUTPUT_DIR ?= ../results/columnar
PGHOST ?= localhost
PGPORT ?= 5432
PGUSER ?= postgres
PGDATABASE ?= orochi_bench
SCALE_FACTOR ?= 1

SRCS := columnar_bench.c
OBJS := $(SRCS:.c=.o)
TARGET := columnar_bench

.PHONY: all clean run setup run-compression run-scan run-filter

all: $(TARGET)

$(TARGET): $(OBJS) $(COMMON_LIB)
	$(CC) $(OBJS) $(COMMON_LIB) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -I../common -c $< -o $@

setup: schema.sql
	@echo "Creating columnar benchmark schema..."
	PGPASSWORD=$(PGPASSWORD) psql -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) -f schema.sql

run-compression: $(TARGET)
	@echo "Running compression benchmark..."
	./$(TARGET) --mode=compression \
		--scale=$(SCALE_FACTOR) \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)

run-scan: $(TARGET)
	@echo "Running scan performance benchmark..."
	./$(TARGET) --mode=scan \
		--scale=$(SCALE_FACTOR) \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)

run-filter: $(TARGET)
	@echo "Running filter pushdown benchmark..."
	./$(TARGET) --mode=filter \
		--scale=$(SCALE_FACTOR) \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)

run: setup run-compression run-scan run-filter
	@echo "Columnar benchmarks complete"

clean:
	rm -f $(OBJS) $(TARGET)
