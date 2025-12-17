/**
 * @file driver_tds.c
 * @brief TDS (Total Dissolved Solids) Sensor driver (via ADC)
 *
 * @deprecated This driver is deprecated. Use analog_sensor.c with
 *             SENSOR_CHAN_TDS and CAL_PRESET_TDS_GENERIC instead.
 *             Migration: src/sensors/analog/analog_sensor.c
 */
#warning "driver_tds.c is deprecated - use analog_sensor.c instead"

#include "common.h"
#include "utils/logger.h"

typedef struct {
    void *adc_handle;
    int adc_channel;
    float vref;
    float temperature;
    float k_value;        // Calibration coefficient
    float cal_scale;
    float cal_offset;
} tds_sensor_t;

#define TDS_DEFAULT_K_VALUE 1.0f
#define TDS_VREF 3.3f

result_t driver_tds_init(void **handle, void *adc_handle, int channel, float vref) {
    tds_sensor_t *sensor = calloc(1, sizeof(tds_sensor_t));
    if (!sensor) return RESULT_NO_MEMORY;
    
    sensor->adc_handle = adc_handle;
    sensor->adc_channel = channel;
    sensor->vref = vref > 0 ? vref : TDS_VREF;
    sensor->temperature = 25.0f;
    sensor->k_value = TDS_DEFAULT_K_VALUE;
    sensor->cal_scale = 1.0f;
    sensor->cal_offset = 0.0f;
    
    *handle = sensor;
    LOG_INFO("TDS sensor initialized on ADC channel %d", channel);
    return RESULT_OK;
}

result_t driver_tds_read(void *handle, float *tds_value) {
    CHECK_NULL(handle);
    CHECK_NULL(tds_value);
    
    tds_sensor_t *sensor = (tds_sensor_t *)handle;
    
    // Would read from ADC
    float voltage = 1.0f;  // Placeholder
    
    // Temperature compensation coefficient
    float compensation = 1.0f + 0.02f * (sensor->temperature - 25.0f);
    float compensated_voltage = voltage / compensation;
    
    // Convert voltage to TDS (ppm)
    // Formula varies by sensor, this is typical for DFRobot TDS sensor
    float tds = (133.42f * compensated_voltage * compensated_voltage * compensated_voltage
               - 255.86f * compensated_voltage * compensated_voltage
               + 857.39f * compensated_voltage) * sensor->k_value;
    
    *tds_value = tds * sensor->cal_scale + sensor->cal_offset;
    *tds_value = MAX(*tds_value, 0.0f);
    
    return RESULT_OK;
}

result_t driver_tds_set_temperature(void *handle, float temperature) {
    CHECK_NULL(handle);
    tds_sensor_t *sensor = (tds_sensor_t *)handle;
    sensor->temperature = temperature;
    return RESULT_OK;
}

result_t driver_tds_calibrate(void *handle, float known_tds, float measured_voltage) {
    CHECK_NULL(handle);
    tds_sensor_t *sensor = (tds_sensor_t *)handle;
    
    float compensation = 1.0f + 0.02f * (sensor->temperature - 25.0f);
    float cv = measured_voltage / compensation;
    float raw_tds = 133.42f * cv * cv * cv - 255.86f * cv * cv + 857.39f * cv;
    
    if (raw_tds > 0) {
        sensor->k_value = known_tds / raw_tds;
        LOG_INFO("TDS calibrated: k=%.4f", sensor->k_value);
    }
    
    return RESULT_OK;
}

void driver_tds_close(void *handle) {
    if (handle) free(handle);
}
