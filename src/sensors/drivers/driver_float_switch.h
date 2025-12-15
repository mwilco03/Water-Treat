#ifndef DRIVER_FLOAT_SWITCH_H
#define DRIVER_FLOAT_SWITCH_H

#include "common.h"
#include "hardware/hw_interface.h"

typedef struct {
    gpio_pin_t gpio;
    bool inverted;
    bool last_state;
} float_switch_t;

result_t float_switch_init(float_switch_t *sw, int gpio_pin, bool inverted);
void float_switch_destroy(float_switch_t *sw);
result_t float_switch_read(float_switch_t *sw, bool *water_detected);
result_t float_switch_wait_for_change(float_switch_t *sw, int timeout_ms);

#endif
