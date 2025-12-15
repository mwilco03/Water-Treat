/**
 * @file driver_turbidity.c
 * @brief Turbidity Sensor driver (via ADC)
 */

#include "common.h"
#include "utils/logger.h"

typedef struct {
    void *adc_handle;
    int adc_channel;
    float vref;
    float clear_water_voltage;  // Voltage in clear water
    float cal_scale;
    float cal_offset;
} turbidity_sensor_t;

#define TURBIDITY_CLEAR_VOLTAGE 4.1f  // Typical clear water voltage

result_t driver_turbidity_init(void **handle, void *adc_handle, int channel, float vref) {
    turbidity_sensor_t *sensor = calloc(1, sizeof(turbidity_sensor_t));
    if (!sensor) return RESULT_NO_MEMORY;
    
    sensor->adc_handle = adc_handle;
    sensor->adc_channel = channel;
    sensor->vref = vref > 0 ? vref : 5.0f;
    sensor->clear_water_voltage = TURBIDITY_CLEAR_VOLTAGE;
    sensor->cal_scale = 1.0f;
    sensor->cal_offset = 0.0f;
    
    *handle = sensor;
    LOG_INFO("Turbidity sensor initialized on ADC channel %d", channel);
    return RESULT_OK;
}

result_t driver_turbidity_read(void *handle, float *ntu_value) {
    CHECK_NULL(handle);
    CHECK_NULL(ntu_value);
    
    turbidity_sensor_t *sensor = (turbidity_sensor_t *)handle;
    
    // Would read from ADC
    float voltage = 3.5f;  // Placeholder
    
    // Convert voltage to NTU (Nephelometric Turbidity Units)
    // Higher voltage = clearer water, lower NTU
    // Formula: NTU = -1120.4 * V^2 + 5742.3 * V - 4352.9 (typical sensor curve)
    float ntu;
    
    if (voltage >= sensor->clear_water_voltage) {
        ntu = 0.0f;
    } else if (voltage < 2.5f) {
        ntu = 3000.0f;  // Very turbid
    } else {
        ntu = -1120.4f * voltage * voltage + 5742.3f * voltage - 4352.9f;
    }
    
    *ntu_value = ntu * sensor->cal_scale + sensor->cal_offset;
    *ntu_value = CLAMP(*ntu_value, 0.0f, 4000.0f);
    
    return RESULT_OK;
}

result_t driver_turbidity_calibrate_clear(void *handle, float clear_voltage) {
    CHECK_NULL(handle);
    turbidity_sensor_t *sensor = (turbidity_sensor_t *)handle;
    sensor->clear_water_voltage = clear_voltage;
    LOG_INFO("Turbidity clear water calibrated: %.3fV", clear_voltage);
    return RESULT_OK;
}

void driver_turbidity_close(void *handle) {
    if (handle) free(handle);
}
