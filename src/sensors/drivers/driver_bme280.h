#ifndef DRIVER_BME280_H
#define DRIVER_BME280_H

#include "common.h"

typedef enum {
    BME280_READ_TEMPERATURE = 0,
    BME280_READ_PRESSURE = 1,
    BME280_READ_HUMIDITY = 2
} bme280_reading_t;

result_t driver_bme280_init(void **handle, int bus, uint8_t address, int reading_type);
result_t driver_bme280_read(void *handle, float *value);
result_t driver_bme280_read_all(void *handle, float *temperature, float *pressure, float *humidity);
void driver_bme280_close(void *handle);

#endif
