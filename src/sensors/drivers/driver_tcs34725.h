#ifndef DRIVER_TCS34725_H
#define DRIVER_TCS34725_H

#include "common.h"
#include "hardware/hw_interface.h"

typedef enum {
    TCS34725_GAIN_1X = 0x00,
    TCS34725_GAIN_4X = 0x01,
    TCS34725_GAIN_16X = 0x02,
    TCS34725_GAIN_60X = 0x03
} tcs34725_gain_t;

typedef enum {
    TCS34725_INTEGRATIONTIME_2_4MS = 0xFF,
    TCS34725_INTEGRATIONTIME_24MS = 0xF6,
    TCS34725_INTEGRATIONTIME_50MS = 0xEB,
    TCS34725_INTEGRATIONTIME_101MS = 0xD5,
    TCS34725_INTEGRATIONTIME_154MS = 0xC0,
    TCS34725_INTEGRATIONTIME_700MS = 0x00
} tcs34725_integration_t;

typedef struct {
    uint16_t c;
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint16_t color_temp;
    uint16_t lux;
} tcs34725_data_t;

typedef struct {
    i2c_device_t i2c;
    tcs34725_gain_t gain;
    tcs34725_integration_t integration_time;
    bool enabled;
} tcs34725_device_t;

result_t tcs34725_init(tcs34725_device_t *dev, int bus, uint8_t address);
void tcs34725_destroy(tcs34725_device_t *dev);
result_t tcs34725_enable(tcs34725_device_t *dev);
result_t tcs34725_disable(tcs34725_device_t *dev);
result_t tcs34725_set_gain(tcs34725_device_t *dev, tcs34725_gain_t gain);
result_t tcs34725_set_integration_time(tcs34725_device_t *dev, tcs34725_integration_t time);
result_t tcs34725_read_raw(tcs34725_device_t *dev, tcs34725_data_t *data);
result_t tcs34725_calculate_color_temperature(tcs34725_data_t *data);
result_t tcs34725_calculate_lux(tcs34725_data_t *data);

#endif
