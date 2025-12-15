#ifndef DRIVER_JSN_SR04T_H
#define DRIVER_JSN_SR04T_H

#include "common.h"
#include "hardware/hw_interface.h"

typedef struct {
    gpio_pin_t trigger;
    gpio_pin_t echo;
    float temperature;
    int timeout_us;
    float last_distance;
    time_t last_read;
} jsn_sr04t_device_t;

result_t jsn_sr04t_init(jsn_sr04t_device_t *dev, int trigger_pin, int echo_pin);
void jsn_sr04t_destroy(jsn_sr04t_device_t *dev);
result_t jsn_sr04t_set_temperature(jsn_sr04t_device_t *dev, float temperature);
result_t jsn_sr04t_read_distance_cm(jsn_sr04t_device_t *dev, float *distance_cm);
result_t jsn_sr04t_read_distance_mm(jsn_sr04t_device_t *dev, float *distance_mm);
result_t jsn_sr04t_read_average(jsn_sr04t_device_t *dev, int samples, float *distance_cm);

#endif
