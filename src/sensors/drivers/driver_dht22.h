#ifndef DRIVER_DHT22_H
#define DRIVER_DHT22_H

#include "common.h"

typedef struct {
    int gpio_pin;
    bool initialized;
    float last_temperature;
    float last_humidity;
    uint64_t last_read_time;
} dht22_t;

result_t dht22_init(dht22_t *dev, int gpio_pin);
result_t dht22_read(dht22_t *dev, float *temperature, float *humidity);
void dht22_close(dht22_t *dev);

/* Driver interface wrapper */
result_t driver_dht22_init(void **handle, int gpio_pin, bool read_humidity);
result_t driver_dht22_read(void *handle, float *value);
result_t driver_dht22_read_both(void *handle, float *temperature, float *humidity);
result_t driver_dht22_set_calibration(void *handle, float scale, float offset);
void driver_dht22_close(void *handle);

#endif
