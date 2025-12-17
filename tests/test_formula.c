/**
 * @file test_formula.c
 * @brief Unit tests for formula evaluator
 */

#include "test_framework.h"
#include "../src/sensors/formula_evaluator.h"

/* Test basic formula initialization */
void test_formula_init(void) {
    formula_evaluator_t eval;
    const char *vars[] = {"x0", "x1"};

    result_t r = formula_evaluator_init(&eval, "x0 + x1", vars, 2);
    TEST_ASSERT(r == RESULT_OK);

    formula_evaluator_destroy(&eval);
}

/* Test simple addition formula */
void test_formula_addition(void) {
    formula_evaluator_t eval;
    const char *vars[] = {"x0", "x1"};

    result_t r = formula_evaluator_init(&eval, "x0 + x1", vars, 2);
    TEST_ASSERT(r == RESULT_OK);

    float inputs[] = {10.0f, 20.0f};
    float result = 0.0f;

    r = formula_evaluator_evaluate(&eval, inputs, &result);
    TEST_ASSERT(r == RESULT_OK);
    TEST_ASSERT_FLOAT_EQ(30.0f, result);

    formula_evaluator_destroy(&eval);
}

/* Test average formula (common in sensor applications) */
void test_formula_average(void) {
    formula_evaluator_t eval;
    const char *vars[] = {"x0", "x1", "x2"};

    /* Average of 3 values */
    result_t r = formula_evaluator_init(&eval, "(x0 + x1 + x2) / 3", vars, 3);
    TEST_ASSERT(r == RESULT_OK);

    float inputs[] = {10.0f, 20.0f, 30.0f};
    float result = 0.0f;

    r = formula_evaluator_evaluate(&eval, inputs, &result);
    TEST_ASSERT(r == RESULT_OK);
    TEST_ASSERT_FLOAT_EQ(20.0f, result);

    formula_evaluator_destroy(&eval);
}

/* Test multiplication */
void test_formula_multiply(void) {
    formula_evaluator_t eval;
    const char *vars[] = {"x0", "x1"};

    result_t r = formula_evaluator_init(&eval, "x0 * x1", vars, 2);
    TEST_ASSERT(r == RESULT_OK);

    float inputs[] = {5.0f, 4.0f};
    float result = 0.0f;

    r = formula_evaluator_evaluate(&eval, inputs, &result);
    TEST_ASSERT(r == RESULT_OK);
    TEST_ASSERT_FLOAT_EQ(20.0f, result);

    formula_evaluator_destroy(&eval);
}

/* Test with single variable */
void test_formula_single_var(void) {
    formula_evaluator_t eval;
    const char *vars[] = {"x0"};

    /* Scale factor application */
    result_t r = formula_evaluator_init(&eval, "x0 * 2.5", vars, 1);
    TEST_ASSERT(r == RESULT_OK);

    float inputs[] = {100.0f};
    float result = 0.0f;

    r = formula_evaluator_evaluate(&eval, inputs, &result);
    TEST_ASSERT(r == RESULT_OK);
    TEST_ASSERT_FLOAT_EQ(250.0f, result);

    formula_evaluator_destroy(&eval);
}

/* Test cleanup without crash */
void test_formula_cleanup(void) {
    formula_evaluator_t eval;
    const char *vars[] = {"x0"};

    formula_evaluator_init(&eval, "x0", vars, 1);
    formula_evaluator_destroy(&eval);

    /* Second destroy should be safe */
    formula_evaluator_destroy(&eval);

    TEST_ASSERT(1);  /* If we get here, no crash */
}

void run_formula_tests(void) {
    TEST_SUITE_BEGIN("Formula Evaluator");

    RUN_TEST(test_formula_init);
    RUN_TEST(test_formula_addition);
    RUN_TEST(test_formula_average);
    RUN_TEST(test_formula_multiply);
    RUN_TEST(test_formula_single_var);
    RUN_TEST(test_formula_cleanup);
}
