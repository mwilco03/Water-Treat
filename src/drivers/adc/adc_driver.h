/**
 * @file adc_driver.h
 * @brief ADC Hardware Abstraction
 *
 * Provides a unified interface to various ADC chips:
 * - ADS1115 (I2C, 16-bit, 4 channels)
 * - MCP3008 (SPI, 10-bit, 8 channels)
 * - Internal ADC (platform-specific)
 */

#ifndef ADC_DRIVER_H
#define ADC_DRIVER_H

#include "common.h"

/* ============================================================================
 * ADC Types
 * ========================================================================== */

typedef enum {
    ADC_TYPE_NONE = 0,
    ADC_TYPE_ADS1115,             // TI ADS1115 - I2C 16-bit
    ADC_TYPE_ADS1015,             // TI ADS1015 - I2C 12-bit
    ADC_TYPE_MCP3008,             // Microchip MCP3008 - SPI 10-bit
    ADC_TYPE_MCP3208,             // Microchip MCP3208 - SPI 12-bit
    ADC_TYPE_INTERNAL,            // Platform internal ADC
} adc_type_t;

/* ADS1115 Gain settings */
typedef enum {
    ADS1115_GAIN_6144MV = 0,      // +/- 6.144V (default)
    ADS1115_GAIN_4096MV = 1,      // +/- 4.096V
    ADS1115_GAIN_2048MV = 2,      // +/- 2.048V
    ADS1115_GAIN_1024MV = 3,      // +/- 1.024V
    ADS1115_GAIN_512MV = 4,       // +/- 0.512V
    ADS1115_GAIN_256MV = 5,       // +/- 0.256V
} ads1115_gain_t;

/* ============================================================================
 * ADC Configuration
 * ========================================================================== */

typedef struct {
    adc_type_t type;

    // I2C settings (ADS1115)
    int i2c_bus;
    uint8_t i2c_address;          // Default: 0x48

    // SPI settings (MCP3008)
    int spi_bus;
    int spi_cs;
    int spi_speed_hz;

    // General
    float vref;                   // Reference voltage
    int sample_rate;              // Samples per second

} adc_config_t;

/* ============================================================================
 * ADC Instance
 * ========================================================================== */

typedef struct adc_instance adc_instance_t;

struct adc_instance {
    adc_config_t config;
    int resolution_bits;
    bool initialized;
    void *priv;
};

/* ============================================================================
 * Public API
 * ========================================================================== */

/**
 * Create and initialize an ADC instance
 */
result_t adc_create(adc_instance_t **adc, const adc_config_t *cfg);
void adc_destroy(adc_instance_t *adc);

/**
 * Read raw ADC value
 * @param channel ADC channel (0-3 for ADS1115, 0-7 for MCP3008)
 * @param raw_value Output raw value
 */
result_t adc_read_raw(adc_instance_t *adc, int channel, int *raw_value);

/**
 * Read ADC value as voltage
 */
result_t adc_read_voltage(adc_instance_t *adc, int channel, float *voltage);

/**
 * Set gain (ADS1115 only)
 */
result_t adc_set_gain(adc_instance_t *adc, ads1115_gain_t gain);

/**
 * Get ADC properties
 */
int adc_get_resolution(adc_instance_t *adc);
float adc_get_vref(adc_instance_t *adc);
int adc_get_channels(adc_instance_t *adc);

/* ============================================================================
 * Convenience: Create backend for sensor_api
 * ========================================================================== */

#include "sensors/sensor_api.h"

/**
 * Wrap an ADC instance as an adc_backend_t for use with analog sensors
 */
result_t adc_create_backend(adc_instance_t *adc, adc_backend_t **backend);

#endif /* ADC_DRIVER_H */
