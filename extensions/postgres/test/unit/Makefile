# Orochi DB - Standalone Unit Test Framework
# Build tests without PostgreSQL dependency

# Compiler and flags
CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -g -O2

# Include paths from pkg-config
PKG_CFLAGS = $(shell pkg-config --cflags liblz4 libzstd openssl 2>/dev/null)

# Include paths
# - test/unit for test_framework.h
# - test/unit/mocks for postgres_mock.h
# - src for Orochi headers (when testing actual modules)
INCLUDES = -I. -I./mocks -I../../src $(PKG_CFLAGS)

# Source directories
SRCDIR = ../../src
TESTDIR = .

# Output
TARGET = run_tests

# Test source files
TEST_SRCS = run_tests.c \
            test_catalog.c \
            test_hypertable.c \
            test_columnar.c \
            test_distribution.c \
            test_executor.c \
            test_auth.c \
            test_jwt.c \
            test_webauthn.c \
            test_magic_link.c \
            test_branch_access.c \
            test_tiered_storage.c \
            test_s3_multipart.c \
            test_s3_operations.c

# Object files for tests
TEST_OBJS = $(TEST_SRCS:.c=.o)

# Module source files to test (add as implemented)
# These will need to be compiled with OROCHI_STANDALONE defined
# MODULE_SRCS = $(SRCDIR)/approx/hyperloglog.c \
#               $(SRCDIR)/approx/tdigest.c

MODULE_OBJS = $(MODULE_SRCS:.c=.o)

# Library paths from pkg-config
PKG_LIBS = $(shell pkg-config --libs liblz4 libzstd openssl 2>/dev/null)

# Libraries needed
LIBS = -lm $(PKG_LIBS)

# Phony targets
.PHONY: all clean test help

# Default target
all: $(TARGET)

# Build the test runner
$(TARGET): $(TEST_OBJS) $(MODULE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# Compile test files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -DOROCHI_STANDALONE -c -o $@ $<

# Compile module files with mock PostgreSQL
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -DOROCHI_STANDALONE -c -o $@ $<

# Run tests
test: $(TARGET)
	./$(TARGET)

# Run tests with verbose output
test-verbose: $(TARGET)
	./$(TARGET) --verbose

# Clean build artifacts
clean:
	rm -f $(TARGET) $(TEST_OBJS) $(MODULE_OBJS)
	rm -f *.o

# Help target
help:
	@echo "Orochi DB Unit Test Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build the test runner (default)"
	@echo "  test         - Run all tests"
	@echo "  test-verbose - Run tests with verbose output"
	@echo "  clean        - Remove build artifacts"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  CC           - C compiler (default: gcc)"
	@echo "  CFLAGS       - Compiler flags"
	@echo ""
	@echo "Usage:"
	@echo "  make         - Build tests"
	@echo "  make test    - Build and run tests"
	@echo "  make clean   - Clean up"

# Dependencies
run_tests.o: run_tests.c test_framework.h mocks/postgres_mock.h
test_catalog.o: test_catalog.c test_framework.h mocks/postgres_mock.h
test_hypertable.o: test_hypertable.c test_framework.h mocks/postgres_mock.h
test_columnar.o: test_columnar.c test_framework.h mocks/postgres_mock.h
test_distribution.o: test_distribution.c test_framework.h mocks/postgres_mock.h
test_executor.o: test_executor.c test_framework.h mocks/postgres_mock.h

# Auth test files
test_auth.o: test_auth.c test_framework.h mocks/postgres_mock.h
test_jwt.o: test_jwt.c test_framework.h mocks/postgres_mock.h
test_webauthn.o: test_webauthn.c test_framework.h mocks/postgres_mock.h
test_magic_link.o: test_magic_link.c test_framework.h mocks/postgres_mock.h
test_branch_access.o: test_branch_access.c test_framework.h mocks/postgres_mock.h

# Tiered storage test files
test_tiered_storage.o: test_tiered_storage.c test_framework.h mocks/postgres_mock.h
test_s3_multipart.o: test_s3_multipart.c test_framework.h mocks/postgres_mock.h
test_s3_operations.o: test_s3_operations.c test_framework.h mocks/postgres_mock.h

# Additional test files (add as implemented)
# test_hyperloglog.o: test_hyperloglog.c test_framework.h mocks/postgres_mock.h
# test_tdigest.o: test_tdigest.c test_framework.h mocks/postgres_mock.h
