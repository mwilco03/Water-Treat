#ifndef DRIVER_DS18B20_H
#define DRIVER_DS18B20_H

#include "common.h"

typedef struct {
    char device_id[20];
    char device_path[128];
    bool initialized;
    float last_temp;
    uint64_t last_read_time;
} ds18b20_t;

result_t ds18b20_init(ds18b20_t *dev, const char *device_id);
result_t ds18b20_read(ds18b20_t *dev, float *temperature);
result_t ds18b20_list_devices(char ***device_ids, int *count);
void ds18b20_close(ds18b20_t *dev);

/* Driver interface wrapper */
result_t driver_ds18b20_init(void **handle, const char *device_id);
result_t driver_ds18b20_read(void *handle, float *value);
result_t driver_ds18b20_set_calibration(void *handle, float scale, float offset);
result_t driver_ds18b20_set_fahrenheit(void *handle, bool fahrenheit);
void driver_ds18b20_close(void *handle);

#endif
