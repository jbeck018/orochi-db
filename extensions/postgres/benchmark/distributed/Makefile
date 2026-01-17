# Distributed Benchmarks for Orochi DB

CC ?= gcc
CFLAGS ?= -std=c11 -O3 -Wall -Wextra
LDFLAGS ?= -lpq -lm -lpthread
COMMON_LIB ?= ../common/libbench_common.a

OUTPUT_DIR ?= ../results/distributed
PGHOST ?= localhost
PGPORT ?= 5432
PGUSER ?= postgres
PGDATABASE ?= orochi_bench
SCALE_FACTOR ?= 1
NUM_SHARDS ?= 8
NUM_CLIENTS ?= 1

SRCS := distributed_bench.c
OBJS := $(SRCS:.c=.o)
TARGET := distributed_bench

.PHONY: all clean run setup run-cross-shard run-local run-rebalance

all: $(TARGET)

$(TARGET): $(OBJS) $(COMMON_LIB)
	$(CC) $(OBJS) $(COMMON_LIB) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -I../common -c $< -o $@

setup: schema.sql
	@echo "Creating distributed benchmark schema..."
	PGPASSWORD=$(PGPASSWORD) psql -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) -f schema.sql

run-cross-shard: $(TARGET)
	@echo "Running cross-shard query benchmark..."
	./$(TARGET) --mode=cross-shard \
		--scale=$(SCALE_FACTOR) \
		--shards=$(NUM_SHARDS) \
		--clients=$(NUM_CLIENTS) \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)

run-local: $(TARGET)
	@echo "Running local shard query benchmark..."
	./$(TARGET) --mode=local \
		--scale=$(SCALE_FACTOR) \
		--shards=$(NUM_SHARDS) \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)

run-rebalance: $(TARGET)
	@echo "Running rebalance benchmark..."
	./$(TARGET) --mode=rebalance \
		--scale=$(SCALE_FACTOR) \
		--shards=$(NUM_SHARDS) \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)

run: setup run-cross-shard run-local
	@echo "Distributed benchmarks complete"

clean:
	rm -f $(OBJS) $(TARGET)
