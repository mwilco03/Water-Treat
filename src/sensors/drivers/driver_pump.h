#ifndef DRIVER_PUMP_H
#define DRIVER_PUMP_H

#include "common.h"
#include "hardware/hw_interface.h"

typedef struct {
    gpio_pin_t gpio;
    bool use_pwm;
    bool is_running;
    int pwm_fd;
    uint32_t period_ns;
    uint32_t duty_ns;
} water_pump_t;

result_t pump_init(water_pump_t *pump, int gpio_pin, bool use_pwm);
void pump_destroy(water_pump_t *pump);
result_t pump_start(water_pump_t *pump);
result_t pump_stop(water_pump_t *pump);
result_t pump_set_speed(water_pump_t *pump, uint8_t speed_percent);
result_t pump_get_state(water_pump_t *pump, bool *is_running);

#endif
