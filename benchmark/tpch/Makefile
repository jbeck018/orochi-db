# TPC-H Style Benchmarks for Orochi DB

CC ?= gcc
CFLAGS ?= -std=c11 -O3 -Wall -Wextra
LDFLAGS ?= -lpq -lm -lpthread
COMMON_LIB ?= ../common/libbench_common.a

# Output directory
OUTPUT_DIR ?= ../results/tpch

# Database settings
PGHOST ?= localhost
PGPORT ?= 5432
PGUSER ?= postgres
PGDATABASE ?= orochi_bench
SCALE_FACTOR ?= 1

SRCS := tpch_bench.c tpch_datagen.c
OBJS := $(SRCS:.c=.o)
TARGET := tpch_bench

.PHONY: all clean run setup generate-data load-data run-queries

all: $(TARGET)

$(TARGET): $(OBJS) $(COMMON_LIB)
	$(CC) $(OBJS) $(COMMON_LIB) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -I../common -c $< -o $@

setup: schema.sql
	@echo "Creating TPC-H schema..."
	PGPASSWORD=$(PGPASSWORD) psql -h $(PGHOST) -p $(PGPORT) -U $(PGUSER) -d $(PGDATABASE) -f schema.sql

generate-data: $(TARGET)
	@echo "Generating TPC-H data (SF=$(SCALE_FACTOR))..."
	./$(TARGET) --generate --scale=$(SCALE_FACTOR) --output=data

load-data: generate-data
	@echo "Loading data into database..."
	./$(TARGET) --load \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--data-dir=data

run-queries: $(TARGET)
	@echo "Running TPC-H queries..."
	./$(TARGET) --benchmark \
		--host=$(PGHOST) --port=$(PGPORT) \
		--user=$(PGUSER) --database=$(PGDATABASE) \
		--scale=$(SCALE_FACTOR) \
		--output=$(OUTPUT_DIR)

run: setup load-data run-queries
	@echo "TPC-H benchmark complete"

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf data/
