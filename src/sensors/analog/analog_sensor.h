/**
 * @file analog_sensor.h
 * @brief Unified Analog Sensor Driver
 *
 * Handles ALL analog probe sensors through ADC:
 * - pH probes
 * - TDS/Conductivity probes
 * - Turbidity sensors
 * - Pressure transducers
 * - Generic 0-5V / 4-20mA sensors
 *
 * The sensor type is determined by calibration, not code.
 */

#ifndef ANALOG_SENSOR_H
#define ANALOG_SENSOR_H

#include "sensors/sensor_api.h"

/* ============================================================================
 * ADC Backend Interface
 *
 * Analog sensors don't talk to hardware directly - they use an ADC backend.
 * This allows the same sensor code to work with different ADC chips.
 * ========================================================================== */

typedef struct adc_backend adc_backend_t;

typedef struct {
    result_t (*init)(adc_backend_t *adc);
    result_t (*read_raw)(adc_backend_t *adc, int channel, int *raw_value);
    result_t (*read_voltage)(adc_backend_t *adc, int channel, float *voltage);
    void (*destroy)(adc_backend_t *adc);
    int (*get_resolution)(adc_backend_t *adc);
    float (*get_vref)(adc_backend_t *adc);
} adc_ops_t;

struct adc_backend {
    const adc_ops_t *ops;
    void *priv;
    int resolution_bits;
    float vref;
};

/* ============================================================================
 * Predefined Calibration Presets
 *
 * Common calibration profiles for typical sensors.
 * Users can override with actual calibration data.
 * ========================================================================== */

// pH probe: typical 0-14 pH, ~0.0V at pH 0, ~3.0V at pH 14
extern const sensor_calibration_t CAL_PRESET_PH_GENERIC;

// TDS probe: typical 0-1000 ppm range
extern const sensor_calibration_t CAL_PRESET_TDS_GENERIC;

// Turbidity: typical 0-1000 NTU
extern const sensor_calibration_t CAL_PRESET_TURBIDITY_GENERIC;

// Pressure: 0.5-4.5V = 0-100 PSI (typical transducer)
extern const sensor_calibration_t CAL_PRESET_PRESSURE_100PSI;

// 4-20mA: Standard industrial transmitter
extern const sensor_calibration_t CAL_PRESET_4_20MA;

/* ============================================================================
 * Analog Sensor Functions
 * ========================================================================== */

/**
 * Create an analog sensor driver
 *
 * @param drv Output: created driver
 * @param cfg Sensor configuration
 * @param adc ADC backend to use for reading
 * @return RESULT_OK on success
 */
result_t analog_sensor_create(sensor_driver_t **drv,
                               const sensor_config_t *cfg,
                               adc_backend_t *adc);

/**
 * Set the ADC backend for an existing sensor
 * Useful when ADC is initialized after sensor configuration
 */
result_t analog_sensor_set_adc(sensor_driver_t *drv, adc_backend_t *adc);

/**
 * Perform two-point calibration with known solutions
 *
 * @param drv Sensor driver
 * @param ref_low Known low reference value (e.g., pH 4.0)
 * @param ref_high Known high reference value (e.g., pH 7.0)
 * @return RESULT_OK on success
 *
 * Call this twice:
 * 1. Place probe in low solution, call with actual_reading=false
 * 2. Place probe in high solution, call with actual_reading=true
 */
result_t analog_sensor_cal_point(sensor_driver_t *drv,
                                  float reference_value,
                                  bool is_high_point);

/**
 * Get the raw ADC value for diagnostics
 */
result_t analog_sensor_read_raw(sensor_driver_t *drv, int *raw_value);

/**
 * Get the voltage reading for diagnostics
 */
result_t analog_sensor_read_voltage(sensor_driver_t *drv, float *voltage);

/* ============================================================================
 * Factory Function (for sensor registry)
 * ========================================================================== */

result_t analog_sensor_factory(sensor_driver_t **drv, const sensor_config_t *cfg);

#endif /* ANALOG_SENSOR_H */
