/**
 * @file test_framework.h
 * @brief Minimal C unit test framework
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>
#include <math.h>

/* Test counters */
static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

/* Current test name for error reporting */
static const char *g_current_test = NULL;

#define TEST_EPSILON 0.0001f

/* Test assertion macros */
#define TEST_ASSERT(expr) do { \
    g_tests_run++; \
    if (!(expr)) { \
        g_tests_failed++; \
        printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

#define TEST_ASSERT_EQ(expected, actual) do { \
    g_tests_run++; \
    if ((expected) != (actual)) { \
        g_tests_failed++; \
        printf("  FAIL: %s:%d: expected %d, got %d\n", __FILE__, __LINE__, (int)(expected), (int)(actual)); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

#define TEST_ASSERT_FLOAT_EQ(expected, actual) do { \
    g_tests_run++; \
    if (fabs((expected) - (actual)) > TEST_EPSILON) { \
        g_tests_failed++; \
        printf("  FAIL: %s:%d: expected %.4f, got %.4f\n", __FILE__, __LINE__, (float)(expected), (float)(actual)); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

#define TEST_ASSERT_STR_EQ(expected, actual) do { \
    g_tests_run++; \
    if (strcmp((expected), (actual)) != 0) { \
        g_tests_failed++; \
        printf("  FAIL: %s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, (expected), (actual)); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

#define TEST_ASSERT_NOT_NULL(ptr) do { \
    g_tests_run++; \
    if ((ptr) == NULL) { \
        g_tests_failed++; \
        printf("  FAIL: %s:%d: expected non-NULL\n", __FILE__, __LINE__); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

#define TEST_ASSERT_NULL(ptr) do { \
    g_tests_run++; \
    if ((ptr) != NULL) { \
        g_tests_failed++; \
        printf("  FAIL: %s:%d: expected NULL\n", __FILE__, __LINE__); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

/* Test runner macros */
#define RUN_TEST(test_func) do { \
    g_current_test = #test_func; \
    printf("Running %s...\n", #test_func); \
    test_func(); \
} while(0)

#define TEST_SUITE_BEGIN(name) \
    printf("\n=== Test Suite: %s ===\n", name)

#define TEST_SUITE_END() \
    printf("\n--- Results ---\n"); \
    printf("Tests run: %d\n", g_tests_run); \
    printf("Passed: %d\n", g_tests_passed); \
    printf("Failed: %d\n", g_tests_failed); \
    return g_tests_failed > 0 ? 1 : 0

#endif /* TEST_FRAMEWORK_H */
