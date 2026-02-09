/*-------------------------------------------------------------------------
 *
 * run_tests.c
 *    Main entry point for Orochi DB unit tests
 *
 * This file runs all registered test suites and reports results.
 * Build with: make -C test/unit
 * Run with: ./test/unit/run_tests
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

/* Test framework */
#include "test_framework.h"

/* Mock PostgreSQL types for standalone compilation */
#include "mocks/postgres_mock.h"

/* ============================================================
 * Test Suite Declarations
 * ============================================================
 * Add new test suite declarations here as:
 *   extern void test_suite_name(void);
 * Then add RUN_TEST_SUITE(test_suite_name) to run_all_tests()
 */

/* Example test suite for framework validation */
static void test_framework_validation(void);

/* HTAP module test suites */
extern void test_catalog(void);
extern void test_hypertable(void);
extern void test_columnar(void);
extern void test_distribution(void);
extern void test_executor(void);

/* Auth module test suites */
extern void test_auth(void);
extern void test_jwt(void);
extern void test_webauthn(void);
extern void test_magic_link(void);
extern void test_branch_access(void);

/* Tiered storage test suites */
extern void test_tiered_storage(void);
extern void test_s3_multipart(void);
extern void test_s3_operations(void);
extern void test_s3_functions(void);

/* Approximate algorithms test suites */
extern void test_approx(void);

/* Vectorized execution test suites */
extern void test_vectorized(void);

/* Compression test suites */
extern void test_compression(void);

/* ============================================================
 * Run All Tests
 * ============================================================ */

static void run_all_tests(void)
{
    /* Framework self-test */
    RUN_TEST_SUITE(test_framework_validation);

    /* HTAP module test suites */
    RUN_TEST_SUITE(test_catalog);
    RUN_TEST_SUITE(test_hypertable);
    RUN_TEST_SUITE(test_columnar);
    RUN_TEST_SUITE(test_distribution);
    RUN_TEST_SUITE(test_executor);

    /* Auth module test suites */
    RUN_TEST_SUITE(test_auth);
    RUN_TEST_SUITE(test_jwt);
    RUN_TEST_SUITE(test_webauthn);
    RUN_TEST_SUITE(test_magic_link);
    RUN_TEST_SUITE(test_branch_access);

    /* Tiered storage test suites */
    RUN_TEST_SUITE(test_tiered_storage);
    RUN_TEST_SUITE(test_s3_multipart);
    RUN_TEST_SUITE(test_s3_operations);
    RUN_TEST_SUITE(test_s3_functions);

    /* Approximate algorithms test suites */
    RUN_TEST_SUITE(test_approx);

    /* Vectorized execution test suites */
    RUN_TEST_SUITE(test_vectorized);

    /* Compression test suites */
    RUN_TEST_SUITE(test_compression);
}

/* ============================================================
 * Framework Validation Tests
 * ============================================================ */

/*
 * Test suite to validate the test framework itself works correctly.
 */
static void test_framework_validation(void)
{
    /* Basic assertion tests */
    TEST_BEGIN("test_assert_true")
        TEST_ASSERT(1 == 1);
        TEST_ASSERT(true);
        TEST_ASSERT(42);
    TEST_END();

    TEST_BEGIN("test_assert_eq_integers")
        TEST_ASSERT_EQ(42, 42);
        TEST_ASSERT_EQ(-1, -1);
        TEST_ASSERT_EQ(0, 0);
        int x = 100;
        TEST_ASSERT_EQ(100, x);
    TEST_END();

    TEST_BEGIN("test_assert_neq")
        TEST_ASSERT_NEQ(1, 2);
        TEST_ASSERT_NEQ(0, 1);
        TEST_ASSERT_NEQ(-1, 1);
    TEST_END();

    TEST_BEGIN("test_assert_near_floats")
        TEST_ASSERT_NEAR(3.14159, 3.14159, 0.00001);
        TEST_ASSERT_NEAR(1.0, 1.0 + 1e-10, 1e-9);
        TEST_ASSERT_NEAR(0.0, 0.0, 0.0);
        double pi = 3.14159265358979;
        TEST_ASSERT_NEAR(pi, 3.14159, 0.00001);
    TEST_END();

    TEST_BEGIN("test_assert_comparisons")
        TEST_ASSERT_GT(10, 5);
        TEST_ASSERT_GTE(10, 10);
        TEST_ASSERT_GTE(10, 5);
        TEST_ASSERT_LT(5, 10);
        TEST_ASSERT_LTE(10, 10);
        TEST_ASSERT_LTE(5, 10);
    TEST_END();

    TEST_BEGIN("test_assert_pointers")
        int x = 42;
        int *ptr = &x;
        TEST_ASSERT_NOT_NULL(ptr);
        int *null_ptr = NULL;
        TEST_ASSERT_NULL(null_ptr);
    TEST_END();

    TEST_BEGIN("test_assert_strings")
        TEST_ASSERT_STR_EQ("hello", "hello");
        TEST_ASSERT_STR_EQ("", "");
        const char *str = "world";
        TEST_ASSERT_STR_EQ("world", str);
    TEST_END();

    TEST_BEGIN("test_assert_memory")
        char buf1[] = {1, 2, 3, 4, 5};
        char buf2[] = {1, 2, 3, 4, 5};
        TEST_ASSERT_MEM_EQ(buf1, buf2, sizeof(buf1));
    TEST_END();

    /* Test memory allocation mocks */
    TEST_BEGIN("test_mock_palloc")
        void *ptr = palloc(100);
        TEST_ASSERT_NOT_NULL(ptr);
        memset(ptr, 0xAA, 100);
        pfree(ptr);

        ptr = palloc0(50);
        TEST_ASSERT_NOT_NULL(ptr);
        /* palloc0 should zero memory */
        char *bytes = (char *)ptr;
        for (int i = 0; i < 50; i++) {
            TEST_ASSERT_EQ(0, bytes[i]);
        }
        pfree(ptr);
    TEST_END();

    /* Test varlena mocks */
    TEST_BEGIN("test_mock_varlena")
        /* Allocate a varlena-style structure */
        size_t data_len = 10;
        size_t total_len = offsetof(varlena, vl_dat) + data_len;
        varlena *v = (varlena *)palloc(total_len);

        SET_VARSIZE(v, total_len);
        TEST_ASSERT_EQ(total_len, VARSIZE(v));

        memset(VARDATA(v), 0x42, data_len);
        TEST_ASSERT_EQ(0x42, ((char *)VARDATA(v))[0]);

        pfree(v);
    TEST_END();

    /* Test list mocks */
    TEST_BEGIN("test_mock_list")
        List *list = NIL;
        TEST_ASSERT_EQ(0, list_length(list));

        int values[] = {10, 20, 30};
        for (int i = 0; i < 3; i++) {
            list = lappend(list, (void *)(intptr_t)values[i]);
        }

        TEST_ASSERT_EQ(3, list_length(list));

        int idx = 0;
        ListCell *cell;
        foreach(cell, list) {
            int val = (int)(intptr_t)lfirst(cell);
            TEST_ASSERT_EQ(values[idx], val);
            idx++;
        }

        /* Clean up - free list cells */
        cell = list->head;
        while (cell) {
            ListCell *next = cell->next;
            free(cell);
            cell = next;
        }
        free(list);
    TEST_END();

    /* Test Datum conversions */
    TEST_BEGIN("test_mock_datum")
        int32 i32 = 12345;
        Datum d = Int32GetDatum(i32);
        TEST_ASSERT_EQ(i32, DatumGetInt32(d));

        int64 i64 = 9876543210LL;
        d = Int64GetDatum(i64);
        TEST_ASSERT_EQ(i64, DatumGetInt64(d));

        bool b = true;
        d = BoolGetDatum(b);
        TEST_ASSERT_EQ(b, DatumGetBool(d));
    TEST_END();
}

/* ============================================================
 * Main Entry Point
 * ============================================================ */

static void print_usage(const char *progname)
{
    printf("Usage: %s [options]\n", progname);
    printf("\nOptions:\n");
    printf("  -v, --verbose    Show detailed test output\n");
    printf("  -h, --help       Show this help message\n");
    printf("\n");
}

int main(int argc, char *argv[])
{
    bool verbose = false;

    /* Parse command line options */
    static struct option long_options[] = {
        {"verbose", no_argument, 0, 'v'},
        {"help",    no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "vh", long_options, NULL)) != -1)
    {
        switch (c)
        {
            case 'v':
                verbose = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    /* Initialize test framework */
    test_init(verbose);

    /* Run all test suites */
    run_all_tests();

    /* Print summary and exit */
    test_summary();

    return test_exit_code();
}
