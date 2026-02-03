#ifndef DRIVER_HX711_H
#define DRIVER_HX711_H

#include "common.h"

result_t driver_hx711_init(void **handle, int dout_pin, int sck_pin, int gain);
result_t driver_hx711_read(void *handle, float *value);
result_t driver_hx711_tare(void *handle);
result_t driver_hx711_calibrate(void *handle, float known_weight);
void driver_hx711_close(void *handle);

#endif
