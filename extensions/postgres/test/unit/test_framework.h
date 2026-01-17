/*-------------------------------------------------------------------------
 *
 * test_framework.h
 *    Standalone unit test framework for Orochi DB
 *
 * This provides a lightweight testing framework that can run without
 * PostgreSQL. It includes assertion macros, test suite management,
 * and summary output.
 *
 * Usage:
 *   TEST_BEGIN("test_name")
 *       // test code
 *       TEST_ASSERT(condition);
 *       TEST_ASSERT_EQ(expected, actual);
 *   TEST_END()
 *
 * Copyright (c) 2024, Orochi DB Contributors
 *
 *-------------------------------------------------------------------------
 */
#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ============================================================
 * Global Test State
 * ============================================================ */

/* Maximum tests and suites */
#define MAX_TESTS       1024
#define MAX_SUITES      64
#define MAX_NAME_LEN    128

/* Test result structure */
typedef struct TestResult
{
    char        name[MAX_NAME_LEN];
    bool        passed;
    char        failure_msg[512];
    double      duration_ms;
} TestResult;

/* Test suite structure */
typedef struct TestSuite
{
    char        name[MAX_NAME_LEN];
    TestResult  tests[MAX_TESTS];
    int         test_count;
    int         passed_count;
    int         failed_count;
} TestSuite;

/* Global test state */
typedef struct TestState
{
    TestSuite   suites[MAX_SUITES];
    int         suite_count;
    int         current_suite;
    int         total_tests;
    int         total_passed;
    int         total_failed;
    bool        verbose;
    clock_t     test_start;
} TestState;

/* Global state instance */
static TestState __test_state = {0};

/* ============================================================
 * Test Framework Initialization and Summary
 * ============================================================ */

/*
 * Initialize the test framework.
 * Call once at the start of main().
 */
static inline void test_init(bool verbose)
{
    memset(&__test_state, 0, sizeof(__test_state));
    __test_state.verbose = verbose;
    printf("========================================\n");
    printf("  Orochi DB Unit Test Framework\n");
    printf("========================================\n\n");
}

/*
 * Begin a new test suite.
 */
static inline void test_suite_begin(const char *name)
{
    if (__test_state.suite_count >= MAX_SUITES)
    {
        fprintf(stderr, "ERROR: Maximum number of test suites exceeded\n");
        exit(1);
    }

    TestSuite *suite = &__test_state.suites[__test_state.suite_count];
    strncpy(suite->name, name, MAX_NAME_LEN - 1);
    suite->name[MAX_NAME_LEN - 1] = '\0';
    suite->test_count = 0;
    suite->passed_count = 0;
    suite->failed_count = 0;

    __test_state.current_suite = __test_state.suite_count;
    __test_state.suite_count++;

    printf("Suite: %s\n", name);
    printf("----------------------------------------\n");
}

/*
 * End current test suite.
 */
static inline void test_suite_end(void)
{
    TestSuite *suite = &__test_state.suites[__test_state.current_suite];
    printf("  Results: %d/%d passed\n\n",
           suite->passed_count, suite->test_count);
}

/*
 * Print summary of all test results.
 */
static inline void test_summary(void)
{
    printf("========================================\n");
    printf("  Test Summary\n");
    printf("========================================\n\n");

    for (int i = 0; i < __test_state.suite_count; i++)
    {
        TestSuite *suite = &__test_state.suites[i];
        printf("  %s: %d/%d passed\n",
               suite->name, suite->passed_count, suite->test_count);

        /* Print failed tests */
        for (int j = 0; j < suite->test_count; j++)
        {
            TestResult *test = &suite->tests[j];
            if (!test->passed)
            {
                printf("    FAIL: %s\n", test->name);
                if (test->failure_msg[0])
                    printf("          %s\n", test->failure_msg);
            }
        }
    }

    printf("\n----------------------------------------\n");
    printf("  TOTAL: %d passed, %d failed, %d total\n",
           __test_state.total_passed,
           __test_state.total_failed,
           __test_state.total_tests);
    printf("----------------------------------------\n");

    if (__test_state.total_failed == 0)
        printf("  All tests PASSED!\n");
    else
        printf("  Some tests FAILED!\n");

    printf("========================================\n");
}

/*
 * Get final exit code (0 = success, 1 = failure).
 */
static inline int test_exit_code(void)
{
    return __test_state.total_failed > 0 ? 1 : 0;
}

/* ============================================================
 * Test Execution Macros
 * ============================================================ */

/* Internal: Record test result */
static inline void __record_test_result(const char *name, bool passed,
                                        const char *failure_msg)
{
    TestSuite *suite = &__test_state.suites[__test_state.current_suite];

    if (suite->test_count >= MAX_TESTS)
    {
        fprintf(stderr, "ERROR: Maximum number of tests exceeded in suite\n");
        exit(1);
    }

    TestResult *test = &suite->tests[suite->test_count];
    strncpy(test->name, name, MAX_NAME_LEN - 1);
    test->name[MAX_NAME_LEN - 1] = '\0';
    test->passed = passed;

    if (failure_msg)
    {
        strncpy(test->failure_msg, failure_msg, sizeof(test->failure_msg) - 1);
        test->failure_msg[sizeof(test->failure_msg) - 1] = '\0';
    }
    else
    {
        test->failure_msg[0] = '\0';
    }

    test->duration_ms = ((double)(clock() - __test_state.test_start) /
                        CLOCKS_PER_SEC) * 1000.0;

    suite->test_count++;
    __test_state.total_tests++;

    if (passed)
    {
        suite->passed_count++;
        __test_state.total_passed++;
        if (__test_state.verbose)
            printf("  [PASS] %s (%.2f ms)\n", name, test->duration_ms);
        else
            printf("  [PASS] %s\n", name);
    }
    else
    {
        suite->failed_count++;
        __test_state.total_failed++;
        printf("  [FAIL] %s\n", name);
        if (failure_msg)
            printf("         %s\n", failure_msg);
    }
}

/*
 * Begin a test case.
 * Usage: TEST_BEGIN("test_name")
 */
#define TEST_BEGIN(name) \
    do { \
        const char *__test_name = (name); \
        bool __test_passed = true; \
        char __failure_msg[512] = {0}; \
        __test_state.test_start = clock(); \
        do {

/*
 * End a test case.
 * Usage: TEST_END()
 */
#define TEST_END() \
        } while(0); \
        __record_test_result(__test_name, __test_passed, \
                            __failure_msg[0] ? __failure_msg : NULL); \
    } while(0)

/* ============================================================
 * Assertion Macros
 * ============================================================ */

/*
 * Assert a condition is true.
 * Usage: TEST_ASSERT(value > 0);
 */
#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Assertion failed: %s (line %d)", #cond, __LINE__); \
        } \
    } while(0)

/*
 * Assert a condition with custom message.
 * Usage: TEST_ASSERT_MSG(value > 0, "value must be positive");
 */
#define TEST_ASSERT_MSG(cond, msg) \
    do { \
        if (!(cond)) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "%s (line %d)", (msg), __LINE__); \
        } \
    } while(0)

/*
 * Assert two values are equal (integers).
 * Usage: TEST_ASSERT_EQ(expected, actual);
 */
#define TEST_ASSERT_EQ(expected, actual) \
    do { \
        long long __expected = (long long)(expected); \
        long long __actual = (long long)(actual); \
        if (__expected != __actual) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Expected %lld, got %lld (line %d)", \
                    __expected, __actual, __LINE__); \
        } \
    } while(0)

/*
 * Assert two values are not equal.
 * Usage: TEST_ASSERT_NEQ(value1, value2);
 */
#define TEST_ASSERT_NEQ(expected, actual) \
    do { \
        long long __expected = (long long)(expected); \
        long long __actual = (long long)(actual); \
        if (__expected == __actual) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Expected not %lld (line %d)", __expected, __LINE__); \
        } \
    } while(0)

/*
 * Assert two floats are approximately equal.
 * Usage: TEST_ASSERT_NEAR(expected, actual, epsilon);
 */
#define TEST_ASSERT_NEAR(expected, actual, epsilon) \
    do { \
        double __expected = (double)(expected); \
        double __actual = (double)(actual); \
        double __epsilon = (double)(epsilon); \
        double __diff = fabs(__expected - __actual); \
        if (__diff > __epsilon) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Expected %.6g (got %.6g, diff %.6g > epsilon %.6g) (line %d)", \
                    __expected, __actual, __diff, __epsilon, __LINE__); \
        } \
    } while(0)

/*
 * Assert value is greater than threshold.
 * Usage: TEST_ASSERT_GT(value, threshold);
 */
#define TEST_ASSERT_GT(value, threshold) \
    do { \
        double __value = (double)(value); \
        double __threshold = (double)(threshold); \
        if (__value <= __threshold) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Expected %.6g > %.6g (line %d)", \
                    __value, __threshold, __LINE__); \
        } \
    } while(0)

/*
 * Assert value is greater than or equal to threshold.
 * Usage: TEST_ASSERT_GTE(value, threshold);
 */
#define TEST_ASSERT_GTE(value, threshold) \
    do { \
        double __value = (double)(value); \
        double __threshold = (double)(threshold); \
        if (__value < __threshold) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Expected %.6g >= %.6g (line %d)", \
                    __value, __threshold, __LINE__); \
        } \
    } while(0)

/*
 * Assert value is less than threshold.
 * Usage: TEST_ASSERT_LT(value, threshold);
 */
#define TEST_ASSERT_LT(value, threshold) \
    do { \
        double __value = (double)(value); \
        double __threshold = (double)(threshold); \
        if (__value >= __threshold) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Expected %.6g < %.6g (line %d)", \
                    __value, __threshold, __LINE__); \
        } \
    } while(0)

/*
 * Assert value is less than or equal to threshold.
 * Usage: TEST_ASSERT_LTE(value, threshold);
 */
#define TEST_ASSERT_LTE(value, threshold) \
    do { \
        double __value = (double)(value); \
        double __threshold = (double)(threshold); \
        if (__value > __threshold) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Expected %.6g <= %.6g (line %d)", \
                    __value, __threshold, __LINE__); \
        } \
    } while(0)

/*
 * Assert pointer is not NULL.
 * Usage: TEST_ASSERT_NOT_NULL(ptr);
 */
#define TEST_ASSERT_NOT_NULL(ptr) \
    do { \
        if ((ptr) == NULL) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Expected non-NULL pointer (line %d)", __LINE__); \
        } \
    } while(0)

/*
 * Assert pointer is NULL.
 * Usage: TEST_ASSERT_NULL(ptr);
 */
#define TEST_ASSERT_NULL(ptr) \
    do { \
        if ((ptr) != NULL) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Expected NULL pointer (line %d)", __LINE__); \
        } \
    } while(0)

/*
 * Assert two strings are equal.
 * Usage: TEST_ASSERT_STR_EQ(expected, actual);
 */
#define TEST_ASSERT_STR_EQ(expected, actual) \
    do { \
        const char *__expected = (expected); \
        const char *__actual = (actual); \
        if (__expected == NULL || __actual == NULL) { \
            if (__expected != __actual) { \
                __test_passed = false; \
                snprintf(__failure_msg, sizeof(__failure_msg), \
                        "String compare with NULL (line %d)", __LINE__); \
            } \
        } else if (strcmp(__expected, __actual) != 0) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Expected \"%s\", got \"%s\" (line %d)", \
                    __expected, __actual, __LINE__); \
        } \
    } while(0)

/*
 * Assert memory regions are equal.
 * Usage: TEST_ASSERT_MEM_EQ(expected, actual, size);
 */
#define TEST_ASSERT_MEM_EQ(expected, actual, size) \
    do { \
        if (memcmp((expected), (actual), (size)) != 0) { \
            __test_passed = false; \
            snprintf(__failure_msg, sizeof(__failure_msg), \
                    "Memory regions differ (line %d)", __LINE__); \
        } \
    } while(0)

/*
 * Assert value is true.
 * Usage: TEST_ASSERT_TRUE(condition);
 */
#define TEST_ASSERT_TRUE(cond) TEST_ASSERT(cond)

/*
 * Assert value is false.
 * Usage: TEST_ASSERT_FALSE(condition);
 */
#define TEST_ASSERT_FALSE(cond) TEST_ASSERT(!(cond))

/*
 * Skip a test with a reason.
 * Usage: TEST_SKIP("reason");
 */
#define TEST_SKIP(reason) \
    do { \
        printf("  [SKIP] %s: %s\n", __test_name, (reason)); \
        __test_passed = true; \
        goto __test_end_skip; \
    } while(0)

/* ============================================================
 * Utility Macros
 * ============================================================ */

/*
 * Run a test function as a suite.
 * Usage: RUN_TEST_SUITE(test_function_name);
 */
#define RUN_TEST_SUITE(func) \
    do { \
        test_suite_begin(#func); \
        func(); \
        test_suite_end(); \
    } while(0)

/*
 * Define a test suite function.
 * Usage: TEST_SUITE(suite_name) { ... }
 */
#define TEST_SUITE(name) static void name(void)

/*
 * Array size helper.
 */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#endif /* TEST_FRAMEWORK_H */
