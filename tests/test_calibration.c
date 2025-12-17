/**
 * @file test_calibration.c
 * @brief Unit tests for sensor calibration and value conversion
 */

#include "test_framework.h"
#include <stdint.h>

/* Simplified calibration function for testing (mirrors sensor_instance.c logic) */
static float apply_calibration(int32_t raw_min, int32_t raw_max,
                               float eng_min, float eng_max,
                               float scale_factor, float offset,
                               int32_t raw_value) {
    /* Avoid division by zero */
    if (raw_max == raw_min) {
        return (float)raw_value * scale_factor + offset;
    }

    /* Linear interpolation */
    float normalized = (float)(raw_value - raw_min) /
                      (float)(raw_max - raw_min);
    float eng_value = normalized * (eng_max - eng_min) + eng_min;

    /* Apply offset and scale */
    eng_value = (eng_value + offset) * scale_factor;

    return eng_value;
}

/* Test basic linear calibration */
void test_calibration_basic(void) {
    /* ADC: 0-4095 -> 0-10V */
    float result = apply_calibration(0, 4095, 0.0f, 10.0f, 1.0f, 0.0f, 2048);

    /* Midpoint should be ~5V */
    TEST_ASSERT_FLOAT_EQ(5.0012f, result);  /* Close to 5 */
}

/* Test pH sensor calibration (typical 0-14 range) */
void test_calibration_ph(void) {
    /* ADC: 0-4095 -> pH 0-14 */
    float ph = apply_calibration(0, 4095, 0.0f, 14.0f, 1.0f, 0.0f, 2048);

    /* Midpoint ~7 pH (neutral) */
    TEST_ASSERT(ph > 6.9f && ph < 7.1f);
}

/* Test temperature calibration with offset */
void test_calibration_temperature(void) {
    /* ADC: 0-4095 -> -40 to 125 degC (DS18B20 range) */
    float temp = apply_calibration(0, 4095, -40.0f, 125.0f, 1.0f, 0.0f, 1638);

    /* ~25% of range = -40 + 0.4 * 165 = 26 degC */
    TEST_ASSERT(temp > 24.0f && temp < 28.0f);
}

/* Test with scale factor */
void test_calibration_scale_factor(void) {
    /* Raw 0-100 -> 0-100, but scaled by 2 */
    float result = apply_calibration(0, 100, 0.0f, 100.0f, 2.0f, 0.0f, 50);

    /* 50 * 2 = 100 */
    TEST_ASSERT_FLOAT_EQ(100.0f, result);
}

/* Test with offset */
void test_calibration_offset(void) {
    /* Raw 0-100 -> 0-100, with offset of 5 */
    float result = apply_calibration(0, 100, 0.0f, 100.0f, 1.0f, 5.0f, 50);

    /* 50 + 5 = 55 */
    TEST_ASSERT_FLOAT_EQ(55.0f, result);
}

/* Test edge cases: minimum value */
void test_calibration_min_value(void) {
    float result = apply_calibration(0, 4095, 0.0f, 10.0f, 1.0f, 0.0f, 0);
    TEST_ASSERT_FLOAT_EQ(0.0f, result);
}

/* Test edge cases: maximum value */
void test_calibration_max_value(void) {
    float result = apply_calibration(0, 4095, 0.0f, 10.0f, 1.0f, 0.0f, 4095);
    TEST_ASSERT_FLOAT_EQ(10.0f, result);
}

/* Test inverted range (e.g., 4-20mA where 4mA is max) */
void test_calibration_inverted(void) {
    /* Some sensors have inverted ranges */
    float result = apply_calibration(0, 4095, 100.0f, 0.0f, 1.0f, 0.0f, 4095);
    TEST_ASSERT_FLOAT_EQ(0.0f, result);

    result = apply_calibration(0, 4095, 100.0f, 0.0f, 1.0f, 0.0f, 0);
    TEST_ASSERT_FLOAT_EQ(100.0f, result);
}

/* Test division by zero protection */
void test_calibration_div_zero(void) {
    /* When raw_min == raw_max, should not crash */
    float result = apply_calibration(100, 100, 0.0f, 10.0f, 1.0f, 0.0f, 50);

    /* Should just apply scale factor */
    TEST_ASSERT_FLOAT_EQ(50.0f, result);
}

/* Test 4-20mA sensor calibration */
void test_calibration_4_20ma(void) {
    /* 4-20mA input, 0-100% engineering units */
    /* Assuming ADC reads 819 at 4mA and 4095 at 20mA */
    float result = apply_calibration(819, 4095, 0.0f, 100.0f, 1.0f, 0.0f, 2457);

    /* 2457 is ~50% of 819-4095 range */
    TEST_ASSERT(result > 48.0f && result < 52.0f);
}

void run_calibration_tests(void) {
    TEST_SUITE_BEGIN("Sensor Calibration");

    RUN_TEST(test_calibration_basic);
    RUN_TEST(test_calibration_ph);
    RUN_TEST(test_calibration_temperature);
    RUN_TEST(test_calibration_scale_factor);
    RUN_TEST(test_calibration_offset);
    RUN_TEST(test_calibration_min_value);
    RUN_TEST(test_calibration_max_value);
    RUN_TEST(test_calibration_inverted);
    RUN_TEST(test_calibration_div_zero);
    RUN_TEST(test_calibration_4_20ma);
}
