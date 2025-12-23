/**
 * @file sensor_api.h
 * @brief Unified Sensor API - follows Linux IIO / Zephyr patterns
 *
 * All sensors implement this interface regardless of underlying hardware.
 * Configuration determines behavior, not separate driver files.
 */

#ifndef SENSOR_API_H
#define SENSOR_API_H

#include "common.h"

/* ============================================================================
 * Sensor Channel Types (what is being measured)
 * ========================================================================== */

typedef enum {
    SENSOR_CHAN_UNKNOWN = 0,

    // Analog measurements (via ADC)
    SENSOR_CHAN_VOLTAGE,
    SENSOR_CHAN_CURRENT,
    SENSOR_CHAN_PH,
    SENSOR_CHAN_TDS,              // Total Dissolved Solids (ppm)
    SENSOR_CHAN_CONDUCTIVITY,     // μS/cm
    SENSOR_CHAN_TURBIDITY,        // NTU
    SENSOR_CHAN_ORP,              // Oxidation-Reduction Potential (mV)
    SENSOR_CHAN_DISSOLVED_O2,     // mg/L
    SENSOR_CHAN_PRESSURE,
    SENSOR_CHAN_LEVEL,            // Tank level (%)

    // Environmental
    SENSOR_CHAN_TEMPERATURE,
    SENSOR_CHAN_HUMIDITY,
    SENSOR_CHAN_AMBIENT_LIGHT,
    SENSOR_CHAN_COLOR_R,
    SENSOR_CHAN_COLOR_G,
    SENSOR_CHAN_COLOR_B,
    SENSOR_CHAN_COLOR_CLEAR,

    // Distance/Position
    SENSOR_CHAN_DISTANCE,
    SENSOR_CHAN_WEIGHT,

    // Digital
    SENSOR_CHAN_BINARY,           // On/Off state

    // Calculated
    SENSOR_CHAN_FORMULA,

    SENSOR_CHAN_MAX
} sensor_channel_t;

/* ============================================================================
 * Sensor Hardware Types (how it connects)
 * ========================================================================== */

typedef enum {
    SENSOR_HW_NONE = 0,
    SENSOR_HW_ADC_INTERNAL,       // Built-in ADC
    SENSOR_HW_ADC_ADS1115,        // External I2C ADC
    SENSOR_HW_ADC_MCP3008,        // External SPI ADC
    SENSOR_HW_I2C,                // Direct I2C sensor (BME280, TCS34725)
    SENSOR_HW_SPI,                // Direct SPI sensor
    SENSOR_HW_ONEWIRE,            // 1-Wire (DS18B20)
    SENSOR_HW_GPIO,               // Direct GPIO (DHT22, float switch)
    SENSOR_HW_GPIO_PWM,           // PWM capable GPIO
    SENSOR_HW_UART,               // Serial sensor
    SENSOR_HW_HTTP,               // Web API polling
    SENSOR_HW_CALCULATED,         // Virtual - computed from other sensors
    SENSOR_HW_STATIC,             // Virtual - fixed value
} sensor_hw_type_t;

/* ============================================================================
 * Calibration Types
 * ========================================================================== */

typedef enum {
    CAL_TYPE_NONE = 0,
    CAL_TYPE_LINEAR,              // y = mx + b
    CAL_TYPE_TWO_POINT,           // Two-point linear calibration
    CAL_TYPE_POLYNOMIAL,          // y = a0 + a1*x + a2*x^2 + ...
    CAL_TYPE_LOOKUP_TABLE,        // Interpolated lookup table
    CAL_TYPE_STEINHART_HART,      // For NTC thermistors
} calibration_type_t;

#define MAX_CAL_POINTS 16
#define MAX_CAL_COEFFICIENTS 6

typedef struct {
    calibration_type_t type;

    union {
        // Linear: y = scale * x + offset
        struct {
            float scale;
            float offset;
        } linear;

        // Two-point: calibrated at two known values
        struct {
            float raw_low, raw_high;
            float ref_low, ref_high;
        } two_point;

        // Polynomial: y = c[0] + c[1]*x + c[2]*x^2 + ...
        struct {
            float coefficients[MAX_CAL_COEFFICIENTS];
            int degree;
        } polynomial;

        // Lookup table: linear interpolation between points
        struct {
            float raw[MAX_CAL_POINTS];
            float eng[MAX_CAL_POINTS];
            int count;
        } lookup;

        // Steinhart-Hart: for thermistors
        struct {
            float a, b, c;
            float series_resistor;
        } steinhart;
    };
} sensor_calibration_t;

/* ============================================================================
 * Sensor Configuration
 * ========================================================================== */

typedef struct {
    // Identity
    int id;
    char name[MAX_NAME_LEN];
    sensor_channel_t channel;
    char unit[16];

    // Hardware binding
    sensor_hw_type_t hw_type;

    union {
        // ADC-based sensors (pH, TDS, turbidity, pressure, etc.)
        struct {
            int adc_channel;
            int adc_gain;           // For programmable gain ADCs
            float vref;             // Reference voltage
            int resolution_bits;
        } adc;

        // I2C sensors
        struct {
            int bus;
            uint8_t address;
        } i2c;

        // SPI sensors
        struct {
            int bus;
            int cs_pin;
        } spi;

        // 1-Wire sensors
        struct {
            int gpio_pin;
            char rom_code[24];      // 64-bit ROM code as hex string
        } onewire;

        // GPIO sensors
        struct {
            int pin;
            bool active_low;
            int pull;               // 0=none, 1=up, 2=down
        } gpio;

        // HTTP polling
        struct {
            char url[256];
            char json_path[128];
            char method[8];
            int timeout_ms;
        } http;

        // Calculated sensors
        struct {
            char formula[256];
            int input_ids[8];
            int input_count;
        } calculated;

        // Static value
        struct {
            float value;
            bool writable;
        } static_val;
    } hw;

    // Calibration
    sensor_calibration_t calibration;

    // Timing
    int poll_rate_ms;
    int timeout_ms;

    // Limits
    float range_min;
    float range_max;
    float filter_alpha;             // EMA filter: 0=none, 0.1=heavy, 0.9=light

} sensor_config_t;

/* ============================================================================
 * Sensor Reading Result
 * ========================================================================== */

typedef enum {
    SENSOR_STATUS_OK = 0,
    SENSOR_STATUS_TIMEOUT,
    SENSOR_STATUS_ERROR,
    SENSOR_STATUS_OUT_OF_RANGE,
    SENSOR_STATUS_NOT_READY,
    SENSOR_STATUS_UNCALIBRATED,
} sensor_status_t;

typedef struct {
    float value;
    float raw_value;                // Pre-calibration value
    sensor_status_t status;
    uint64_t timestamp_ms;
} sensor_result_t;

/* ============================================================================
 * Unified Sensor Driver Interface
 * ========================================================================== */

// Forward declaration
typedef struct sensor_driver sensor_driver_t;

// Driver operations vtable
typedef struct {
    result_t (*init)(sensor_driver_t *drv, const sensor_config_t *cfg);
    result_t (*read)(sensor_driver_t *drv, sensor_result_t *reading);
    result_t (*write)(sensor_driver_t *drv, float value);  // For outputs
    result_t (*calibrate)(sensor_driver_t *drv, const sensor_calibration_t *cal);
    void     (*destroy)(sensor_driver_t *drv);
} sensor_ops_t;

struct sensor_driver {
    sensor_config_t config;
    sensor_result_t last_reading;
    void *priv;                     // Driver-specific private data
    const sensor_ops_t *ops;
};

/* ============================================================================
 * Sensor API Functions
 * ========================================================================== */

// Create sensor from configuration
result_t sensor_create(sensor_driver_t **drv, const sensor_config_t *cfg);
void sensor_destroy(sensor_driver_t *drv);

// Read sensor value
result_t sensor_read(sensor_driver_t *drv, sensor_result_t *reading);
result_t sensor_read_cached(sensor_driver_t *drv, sensor_result_t *reading);

// For outputs (pumps, valves)
result_t sensor_write(sensor_driver_t *drv, float value);

// Calibration
result_t sensor_calibrate(sensor_driver_t *drv, const sensor_calibration_t *cal);
result_t sensor_set_two_point_cal(sensor_driver_t *drv,
                                   float raw_low, float ref_low,
                                   float raw_high, float ref_high);

// Utilities
float sensor_apply_calibration(const sensor_calibration_t *cal, float raw);
const char* sensor_channel_name(sensor_channel_t chan);
const char* sensor_status_string(sensor_status_t status);

/* ============================================================================
 * Driver Registration (for extensibility)
 * ========================================================================== */

typedef result_t (*sensor_driver_factory_t)(sensor_driver_t **drv, const sensor_config_t *cfg);

result_t sensor_register_driver(sensor_hw_type_t hw_type, sensor_driver_factory_t factory);
result_t sensor_unregister_driver(sensor_hw_type_t hw_type);

#endif /* SENSOR_API_H */
