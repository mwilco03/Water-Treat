/**
 * @file test_main.c
 * @brief Main test runner for Water Treatment RTU unit tests
 */

#include <stdio.h>
#include "test_framework.h"

/* External test suite runners */
extern void run_formula_tests(void);
extern void run_calibration_tests(void);
extern void run_alarm_tests(void);
extern void run_profinet_data_tests(void);
extern void run_config_tests(void);

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("===============================================\n");
    printf("  Water Treatment RTU - Unit Test Suite\n");
    printf("===============================================\n");

    /* Run all test suites */
    run_formula_tests();
    run_calibration_tests();
    run_alarm_tests();
    run_profinet_data_tests();
    run_config_tests();

    /* Print final summary */
    printf("\n===============================================\n");
    printf("  FINAL RESULTS\n");
    printf("===============================================\n");
    printf("Total tests run: %d\n", g_tests_run);
    printf("Passed: %d\n", g_tests_passed);
    printf("Failed: %d\n", g_tests_failed);
    printf("===============================================\n");

    if (g_tests_failed > 0) {
        printf("\nSOME TESTS FAILED!\n");
        return 1;
    } else {
        printf("\nALL TESTS PASSED!\n");
        return 0;
    }
}
