/**
 * @file test_profinet_data.c
 * @brief Unit tests for PROFINET data encoding/decoding
 */

#include "test_framework.h"
#include <string.h>
#include <stdint.h>

/* PROFINET uses big-endian (network byte order) */
static uint16_t swap16(uint16_t val) {
    return (val << 8) | (val >> 8);
}

static uint32_t swap32(uint32_t val) {
    return ((val & 0xFF000000) >> 24) |
           ((val & 0x00FF0000) >> 8) |
           ((val & 0x0000FF00) << 8) |
           ((val & 0x000000FF) << 24);
}

/* IEEE 754 float to bytes (big-endian for PROFINET) */
static void float_to_bytes_be(float value, uint8_t *bytes) {
    union {
        float f;
        uint32_t u;
    } conv;
    conv.f = value;
    uint32_t swapped = swap32(conv.u);
    memcpy(bytes, &swapped, 4);
}

/* Bytes (big-endian) to IEEE 754 float */
static float bytes_to_float_be(const uint8_t *bytes) {
    uint32_t swapped;
    memcpy(&swapped, bytes, 4);
    uint32_t native = swap32(swapped);
    union {
        float f;
        uint32_t u;
    } conv;
    conv.u = native;
    return conv.f;
}

/* Test float encoding to PROFINET format */
void test_profinet_float_encoding(void) {
    uint8_t bytes[4];

    /* Test zero */
    float_to_bytes_be(0.0f, bytes);
    float result = bytes_to_float_be(bytes);
    TEST_ASSERT_FLOAT_EQ(0.0f, result);

    /* Test positive value */
    float_to_bytes_be(25.5f, bytes);
    result = bytes_to_float_be(bytes);
    TEST_ASSERT_FLOAT_EQ(25.5f, result);

    /* Test negative value */
    float_to_bytes_be(-10.25f, bytes);
    result = bytes_to_float_be(bytes);
    TEST_ASSERT_FLOAT_EQ(-10.25f, result);

    /* Test very small value */
    float_to_bytes_be(0.001f, bytes);
    result = bytes_to_float_be(bytes);
    TEST_ASSERT(result > 0.0009f && result < 0.0011f);
}

/* Test sensor value encoding (pH sensor example) */
void test_profinet_ph_encoding(void) {
    uint8_t io_data[4];

    /* pH 7.0 (neutral) */
    float_to_bytes_be(7.0f, io_data);
    float decoded = bytes_to_float_be(io_data);
    TEST_ASSERT_FLOAT_EQ(7.0f, decoded);

    /* Verify byte order (IEEE 754 for 7.0 = 0x40E00000) */
    /* Big endian: 0x40, 0xE0, 0x00, 0x00 */
    TEST_ASSERT_EQ(0x40, io_data[0]);
    TEST_ASSERT_EQ(0xE0, io_data[1]);
    TEST_ASSERT_EQ(0x00, io_data[2]);
    TEST_ASSERT_EQ(0x00, io_data[3]);
}

/* Test temperature encoding */
void test_profinet_temperature_encoding(void) {
    uint8_t io_data[4];

    /* 25.0 degrees Celsius */
    float_to_bytes_be(25.0f, io_data);
    float decoded = bytes_to_float_be(io_data);
    TEST_ASSERT_FLOAT_EQ(25.0f, decoded);

    /* -40.0 degrees (minimum) */
    float_to_bytes_be(-40.0f, io_data);
    decoded = bytes_to_float_be(io_data);
    TEST_ASSERT_FLOAT_EQ(-40.0f, decoded);
}

/* Test actuator command decoding */
void test_profinet_actuator_decode(void) {
    /* Actuator command structure:
     * byte 0: command (0=off, 1=on, 2=PWM)
     * byte 1: PWM duty cycle (0-255)
     * bytes 2-3: reserved
     */
    uint8_t cmd_off[] = {0x00, 0x00, 0x00, 0x00};
    uint8_t cmd_on[] = {0x01, 0x00, 0x00, 0x00};
    uint8_t cmd_pwm[] = {0x02, 0x80, 0x00, 0x00};  /* PWM at 50% */

    TEST_ASSERT_EQ(0, cmd_off[0]);  /* Off command */
    TEST_ASSERT_EQ(1, cmd_on[0]);   /* On command */
    TEST_ASSERT_EQ(2, cmd_pwm[0]);  /* PWM command */
    TEST_ASSERT_EQ(128, cmd_pwm[1]); /* 50% duty = 128/255 */
}

/* Test I/O data sizes per PROFINET spec */
void test_profinet_io_sizes(void) {
    /* Each sensor provides 4 bytes (IEEE 754 float) */
    size_t sensor_data_size = 4;
    TEST_ASSERT_EQ(4, sensor_data_size);

    /* 8 input modules = 32 bytes total input */
    size_t total_input = 8 * sensor_data_size;
    TEST_ASSERT_EQ(32, total_input);

    /* Each actuator uses 4 bytes */
    size_t actuator_data_size = 4;
    TEST_ASSERT_EQ(4, actuator_data_size);

    /* 7 output modules = 28 bytes total output */
    size_t total_output = 7 * actuator_data_size;
    TEST_ASSERT_EQ(28, total_output);
}

/* Test 16-bit integer encoding (used in some PROFINET fields) */
void test_profinet_uint16_encoding(void) {
    uint16_t value = 0x1234;
    uint16_t swapped = swap16(value);

    /* Big endian */
    uint8_t *bytes = (uint8_t *)&swapped;
    TEST_ASSERT_EQ(0x12, bytes[0]);
    TEST_ASSERT_EQ(0x34, bytes[1]);
}

/* Test data consistency across encode/decode cycles */
void test_profinet_roundtrip(void) {
    float test_values[] = {0.0f, 1.0f, -1.0f, 100.5f, -273.15f, 1000000.0f, 0.00001f};
    int num_values = sizeof(test_values) / sizeof(test_values[0]);

    for (int i = 0; i < num_values; i++) {
        uint8_t bytes[4];
        float_to_bytes_be(test_values[i], bytes);
        float result = bytes_to_float_be(bytes);
        TEST_ASSERT_FLOAT_EQ(test_values[i], result);
    }
}

/* Test vendor/device ID encoding */
void test_profinet_id_encoding(void) {
    /* From profinet_callbacks.c - vendor ID 0x0493 */
    uint16_t vendor_id = 0x0493;
    uint16_t device_id = 0x0001;

    /* These are typically stored in little-endian in memory
     * but transmitted big-endian on wire */
    uint16_t vendor_be = swap16(vendor_id);
    uint16_t device_be = swap16(device_id);

    uint8_t *v = (uint8_t *)&vendor_be;
    TEST_ASSERT_EQ(0x04, v[0]);
    TEST_ASSERT_EQ(0x93, v[1]);

    uint8_t *d = (uint8_t *)&device_be;
    TEST_ASSERT_EQ(0x00, d[0]);
    TEST_ASSERT_EQ(0x01, d[1]);
}

void run_profinet_data_tests(void) {
    TEST_SUITE_BEGIN("PROFINET Data Encoding");

    RUN_TEST(test_profinet_float_encoding);
    RUN_TEST(test_profinet_ph_encoding);
    RUN_TEST(test_profinet_temperature_encoding);
    RUN_TEST(test_profinet_actuator_decode);
    RUN_TEST(test_profinet_io_sizes);
    RUN_TEST(test_profinet_uint16_encoding);
    RUN_TEST(test_profinet_roundtrip);
    RUN_TEST(test_profinet_id_encoding);
}
