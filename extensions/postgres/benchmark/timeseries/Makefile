# Time-Series Benchmarks for Orochi DB

CC ?= gcc
CFLAGS ?= -std=c11 -O3 -Wall -Wextra
LDFLAGS ?= -lpq -lm -lpthread
COMMON_LIB ?= ../common/libbench_common.a

OUTPUT_DIR ?= ../results/timeseries
PGHOST ?= localhost
PGPORT ?= 5432
PGUSER ?= postgres
PGDATABASE ?= orochi_bench
SCALE_FACTOR ?= 1
NUM_CLIENTS ?= 1

SRCS := ts_bench.c
OBJS := $(SRCS:.c=.o)
TARGET := ts_bench

.PHONY: all clean run setup run-insert run-query run-chunk

all: $(TARGET)

$(TARGET): $(OBJS) $(COMMON_LIB)
	$(CC) $(OBJS) $(COMMON_LIB) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -I../common -c $< -o $@

setup: schema.sql
	@echo "Creating time-series schema..."
	PGPASSWORD=$(PGPASSWORD) psql -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) -f schema.sql

run-insert: $(TARGET)
	@echo "Running INSERT benchmark..."
	./$(TARGET) --mode=insert \
		--scale=$(SCALE_FACTOR) \
		--clients=$(NUM_CLIENTS) \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)

run-query: $(TARGET)
	@echo "Running query benchmark..."
	./$(TARGET) --mode=query \
		--scale=$(SCALE_FACTOR) \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)

run-chunk: $(TARGET)
	@echo "Running chunk management benchmark..."
	./$(TARGET) --mode=chunk \
		--scale=$(SCALE_FACTOR) \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--output=$(OUTPUT_DIR)

run: setup run-insert run-query run-chunk
	@echo "Time-series benchmarks complete"

clean:
	rm -f $(OBJS) $(TARGET)
