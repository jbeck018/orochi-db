# Orochi DB Unit Test Framework

This document describes the standalone unit testing framework for Orochi DB. Tests run without requiring a PostgreSQL installation.

## Architecture Overview

```
test/unit/
    test_framework.h     # Core test framework (assertions, suites, reporting)
    mocks/
        postgres_mock.h  # Mock PostgreSQL types for standalone compilation
    run_tests.c          # Main entry point - runs all test suites
    test_*.c             # Individual test suite files
    Makefile             # Build system
```

### Core Components

| File | Purpose |
|------|---------|
| `test_framework.h` | Provides TEST_BEGIN/TEST_END macros, assertion macros, suite management, and result reporting |
| `mocks/postgres_mock.h` | Mock implementations of PostgreSQL types (Oid, Datum, varlena, List, palloc/pfree, etc.) |
| `run_tests.c` | Main entry point that calls all registered test suites |

## How to Run Tests

```bash
# Build and run tests
cd test/unit
make test

# Run with verbose output (shows timing)
make test-verbose

# Build only
make

# Clean build artifacts
make clean
```

### Command Line Options

```bash
./run_tests --help        # Show usage
./run_tests --verbose     # Show detailed output with timing
./run_tests -v            # Short form
```

## Adding New Tests

### Step 1: Create Test File

Create a new file `test_<module>.c`:

```c
/*-------------------------------------------------------------------------
 *
 * test_mymodule.c
 *    Unit tests for Orochi DB mymodule functionality
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* ============================================================
 * Mock State (if needed)
 * ============================================================ */

static void mock_init(void)
{
    /* Initialize mock state */
}

static void mock_cleanup(void)
{
    /* Clean up mock state */
}

/* ============================================================
 * Test Cases
 * ============================================================ */

static void test_basic_functionality(void)
{
    TEST_BEGIN("basic_functionality")
        mock_init();

        /* Test code here */
        int result = 42;
        TEST_ASSERT_EQ(42, result);

        mock_cleanup();
    TEST_END();
}

static void test_edge_cases(void)
{
    TEST_BEGIN("edge_cases")
        /* Test edge cases */
        TEST_ASSERT_NOT_NULL(palloc(100));
    TEST_END();
}

/* ============================================================
 * Test Suite Entry Point
 * ============================================================ */

void test_mymodule(void)
{
    test_basic_functionality();
    test_edge_cases();
    /* Add more test functions here */
}
```

### Step 2: Register in run_tests.c

Add your suite declaration and registration:

```c
/* Add declaration at top with other extern declarations */
extern void test_mymodule(void);

/* Add to run_all_tests() function */
static void run_all_tests(void)
{
    /* ... existing suites ... */
    RUN_TEST_SUITE(test_mymodule);
}
```

### Step 3: Update Makefile

Add your test file to `TEST_SRCS`:

```makefile
TEST_SRCS = run_tests.c \
            test_catalog.c \
            test_mymodule.c \    # Add this line
            ...

# Add dependency rule at bottom
test_mymodule.o: test_mymodule.c test_framework.h mocks/postgres_mock.h
```

## Test Assertion Macros

### Basic Assertions

| Macro | Usage | Description |
|-------|-------|-------------|
| `TEST_ASSERT(cond)` | `TEST_ASSERT(ptr != NULL)` | Assert condition is true |
| `TEST_ASSERT_MSG(cond, msg)` | `TEST_ASSERT_MSG(x > 0, "x must be positive")` | Assert with custom message |
| `TEST_ASSERT_TRUE(cond)` | `TEST_ASSERT_TRUE(flag)` | Alias for TEST_ASSERT |
| `TEST_ASSERT_FALSE(cond)` | `TEST_ASSERT_FALSE(is_empty)` | Assert condition is false |

### Equality Assertions

| Macro | Usage | Description |
|-------|-------|-------------|
| `TEST_ASSERT_EQ(expected, actual)` | `TEST_ASSERT_EQ(42, result)` | Assert integers equal |
| `TEST_ASSERT_NEQ(val1, val2)` | `TEST_ASSERT_NEQ(a, b)` | Assert integers not equal |
| `TEST_ASSERT_STR_EQ(expected, actual)` | `TEST_ASSERT_STR_EQ("hello", str)` | Assert strings equal |
| `TEST_ASSERT_MEM_EQ(exp, act, size)` | `TEST_ASSERT_MEM_EQ(buf1, buf2, 10)` | Assert memory regions equal |

### Numeric Comparisons

| Macro | Usage | Description |
|-------|-------|-------------|
| `TEST_ASSERT_GT(val, threshold)` | `TEST_ASSERT_GT(count, 0)` | Assert value > threshold |
| `TEST_ASSERT_GTE(val, threshold)` | `TEST_ASSERT_GTE(count, 1)` | Assert value >= threshold |
| `TEST_ASSERT_LT(val, threshold)` | `TEST_ASSERT_LT(count, 100)` | Assert value < threshold |
| `TEST_ASSERT_LTE(val, threshold)` | `TEST_ASSERT_LTE(count, 99)` | Assert value <= threshold |
| `TEST_ASSERT_NEAR(exp, act, epsilon)` | `TEST_ASSERT_NEAR(3.14, pi, 0.01)` | Assert floats approximately equal |

### Pointer Assertions

| Macro | Usage | Description |
|-------|-------|-------------|
| `TEST_ASSERT_NOT_NULL(ptr)` | `TEST_ASSERT_NOT_NULL(result)` | Assert pointer is not NULL |
| `TEST_ASSERT_NULL(ptr)` | `TEST_ASSERT_NULL(error)` | Assert pointer is NULL |

### Test Control

| Macro | Usage | Description |
|-------|-------|-------------|
| `TEST_SKIP(reason)` | `TEST_SKIP("Not implemented yet")` | Skip test with reason |

## Test Structure Pattern

```c
static void test_function_name(void)
{
    TEST_BEGIN("descriptive_test_name")
        /* 1. Setup / Arrange */
        mock_init();
        MockType *obj = mock_create_object(1001, "test");

        /* 2. Execute / Act */
        int result = function_under_test(obj);

        /* 3. Verify / Assert */
        TEST_ASSERT_EQ(expected_value, result);
        TEST_ASSERT_NOT_NULL(obj->field);

        /* 4. Cleanup */
        mock_cleanup();
    TEST_END();
}
```

## Mock System (postgres_mock.h)

The mock system provides standalone implementations of PostgreSQL types.

### Available Types

| Type | Description |
|------|-------------|
| `Oid` | Object identifier (unsigned int) |
| `Datum` | Universal value type (uintptr_t) |
| `int8/16/32/64` | Fixed-width integers |
| `uint8/16/32/64` | Unsigned fixed-width integers |
| `TimestampTz` | Timestamp with timezone (int64) |
| `varlena`, `bytea`, `text` | Variable-length data types |
| `List`, `ListCell` | Linked list |

### Memory Functions

```c
void *palloc(size_t size);      /* Allocate memory */
void *palloc0(size_t size);     /* Allocate zeroed memory */
void pfree(void *ptr);          /* Free memory */
void *repalloc(void *ptr, size_t size);  /* Reallocate */
```

### Datum Conversion Macros

```c
/* Integer conversions */
Int32GetDatum(x)    DatumGetInt32(x)
Int64GetDatum(x)    DatumGetInt64(x)

/* Pointer conversions */
PointerGetDatum(x)  DatumGetPointer(x)

/* Boolean conversions */
BoolGetDatum(x)     DatumGetBool(x)

/* Float conversions */
Float8GetDatum(x)   DatumGetFloat8(x)
```

### Varlena Macros

```c
SET_VARSIZE(ptr, len)       /* Set varlena length */
VARSIZE(ptr)                /* Get total size including header */
VARDATA(ptr)                /* Get pointer to data portion */
VARSIZE_ANY_EXHDR(ptr)      /* Get data size excluding header */
```

### List Operations

```c
List *list = NIL;                           /* Empty list */
list = lappend(list, ptr);                  /* Append element */
int len = list_length(list);                /* Get length */

ListCell *cell;
foreach(cell, list) {                       /* Iterate */
    void *value = lfirst(cell);
}
```

## Naming Conventions

### File Names

- Test files: `test_<module>.c` (e.g., `test_catalog.c`, `test_hypertable.c`)
- Module tests should match source module names

### Function Names

- Test suite entry: `void test_<module>(void)` (e.g., `test_catalog`)
- Individual tests: `static void test_<what>_<scenario>(void)`

### Test Names (in TEST_BEGIN)

- Use snake_case: `"descriptive_test_name"`
- Be specific: `"shard_lookup_by_hash_value"` not `"test1"`
- Include the operation: `"create"`, `"delete"`, `"update"`, `"lookup"`

### Examples

```c
/* Good test names */
TEST_BEGIN("table_registration")
TEST_BEGIN("shard_mapping_lookup")
TEST_BEGIN("chunk_compression_toggle")
TEST_BEGIN("retention_policy_creation")
TEST_BEGIN("drop_chunks_older_than")

/* Bad test names */
TEST_BEGIN("test1")
TEST_BEGIN("it_works")
TEST_BEGIN("foo")
```

## Test Suite Organization

Each test file follows this pattern:

```c
/* 1. Header comment with description */

/* 2. Includes */
#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* 3. Constants and types (mock data structures) */
#define MAX_ITEMS 100
typedef struct MockItem { ... } MockItem;

/* 4. Mock state and functions */
static MockItem mock_items[MAX_ITEMS];
static void mock_init(void) { ... }
static void mock_cleanup(void) { ... }

/* 5. Individual test functions (static) */
static void test_create(void) { ... }
static void test_lookup(void) { ... }
static void test_delete(void) { ... }

/* 6. Suite entry point (extern, called from run_tests.c) */
void test_modulename(void)
{
    test_create();
    test_lookup();
    test_delete();
}
```

## Example: Complete Test Suite

```c
/*-------------------------------------------------------------------------
 * test_example.c - Example test suite for documentation
 *-------------------------------------------------------------------------
 */

#include "test_framework.h"
#include "mocks/postgres_mock.h"

/* Mock state */
#define MAX_ITEMS 10
static int mock_items[MAX_ITEMS];
static int mock_count = 0;

static void mock_init(void)
{
    memset(mock_items, 0, sizeof(mock_items));
    mock_count = 0;
}

static int mock_add_item(int value)
{
    if (mock_count >= MAX_ITEMS) return -1;
    mock_items[mock_count++] = value;
    return mock_count - 1;
}

static int mock_get_item(int index)
{
    if (index < 0 || index >= mock_count) return -1;
    return mock_items[index];
}

/* Tests */
static void test_add_item(void)
{
    TEST_BEGIN("add_item_basic")
        mock_init();

        int idx = mock_add_item(42);
        TEST_ASSERT_EQ(0, idx);
        TEST_ASSERT_EQ(1, mock_count);
        TEST_ASSERT_EQ(42, mock_items[0]);
    TEST_END();
}

static void test_get_item(void)
{
    TEST_BEGIN("get_item_valid_index")
        mock_init();
        mock_add_item(10);
        mock_add_item(20);

        TEST_ASSERT_EQ(10, mock_get_item(0));
        TEST_ASSERT_EQ(20, mock_get_item(1));
    TEST_END();

    TEST_BEGIN("get_item_invalid_index")
        mock_init();
        mock_add_item(10);

        TEST_ASSERT_EQ(-1, mock_get_item(-1));
        TEST_ASSERT_EQ(-1, mock_get_item(99));
    TEST_END();
}

static void test_capacity_limit(void)
{
    TEST_BEGIN("add_item_at_capacity")
        mock_init();

        for (int i = 0; i < MAX_ITEMS; i++) {
            int idx = mock_add_item(i);
            TEST_ASSERT_EQ(i, idx);
        }

        /* Should fail when at capacity */
        int idx = mock_add_item(999);
        TEST_ASSERT_EQ(-1, idx);
        TEST_ASSERT_EQ(MAX_ITEMS, mock_count);
    TEST_END();
}

/* Suite entry point */
void test_example(void)
{
    test_add_item();
    test_get_item();
    test_capacity_limit();
}
```

## Output Format

### Normal Output

```
========================================
  Orochi DB Unit Test Framework
========================================

Suite: test_framework_validation
----------------------------------------
  [PASS] test_assert_true
  [PASS] test_assert_eq_integers
  [PASS] test_assert_near_floats
  Results: 3/3 passed

Suite: test_catalog
----------------------------------------
  [PASS] catalog_initialization
  [PASS] table_registration
  [FAIL] table_lookup_by_oid
         Expected 1001, got 0 (line 145)
  Results: 2/3 passed

========================================
  Test Summary
========================================

  test_framework_validation: 3/3 passed
  test_catalog: 2/3 passed
    FAIL: table_lookup_by_oid
          Expected 1001, got 0 (line 145)

----------------------------------------
  TOTAL: 5 passed, 1 failed, 6 total
----------------------------------------
  Some tests FAILED!
========================================
```

### Verbose Output (--verbose)

Includes timing information:

```
  [PASS] test_assert_true (0.01 ms)
  [PASS] test_assert_eq_integers (0.02 ms)
```

## Best Practices

1. **One assertion focus per test**: Each TEST_BEGIN/TEST_END block should test one specific behavior
2. **Initialize and cleanup**: Always call mock_init() at start and mock_cleanup() at end
3. **Test edge cases**: Include tests for NULL, empty, boundary conditions
4. **Descriptive names**: Test names should describe the scenario being tested
5. **Independent tests**: Tests should not depend on state from other tests
6. **Keep tests fast**: Unit tests should complete in milliseconds

## Troubleshooting

### Common Issues

| Issue | Solution |
|-------|----------|
| Undefined reference to `test_xxx` | Add `extern void test_xxx(void);` in run_tests.c |
| Missing postgres types | Include `mocks/postgres_mock.h` |
| Linker errors | Add .c file to TEST_SRCS in Makefile |
| Test not running | Add `RUN_TEST_SUITE(test_xxx)` in run_all_tests() |

### Build Errors

```bash
# Missing math library
# Add -lm to LIBS in Makefile

# Missing include
# Check INCLUDES path in Makefile: -I. -I./mocks -I../../src
```
