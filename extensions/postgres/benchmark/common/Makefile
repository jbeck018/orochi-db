# Common Benchmark Library for Orochi DB
#
# Builds the shared benchmark utilities library

CC ?= gcc
AR ?= ar
ARFLAGS ?= rcs

# Detect PostgreSQL paths using pg_config
PG_CONFIG ?= pg_config
PG_INCLUDEDIR := $(shell $(PG_CONFIG) --includedir 2>/dev/null || echo "/opt/homebrew/include/postgresql@14")

CFLAGS ?= -std=c11 -O3 -Wall -Wextra -Wpedantic -fPIC -I$(PG_INCLUDEDIR)

# Debug build flags
ifdef DEBUG
CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Wpedantic -fPIC -DDEBUG
endif

# Source and output
SRCS := bench_common.c
OBJS := $(SRCS:.c=.o)
TARGET := libbench_common.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

%.o: %.c bench_common.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

# Install headers (for external use)
install: $(TARGET)
	@echo "Common benchmark library built: $(TARGET)"
