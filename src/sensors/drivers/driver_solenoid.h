#ifndef DRIVER_SOLENOID_H
#define DRIVER_SOLENOID_H

#include "common.h"
#include "hardware/hw_interface.h"

typedef struct {
    gpio_pin_t gpio;
    bool inverted;
    bool is_open;
} solenoid_valve_t;

result_t solenoid_init(solenoid_valve_t *valve, int gpio_pin, bool inverted);
void solenoid_destroy(solenoid_valve_t *valve);
result_t solenoid_open(solenoid_valve_t *valve);
result_t solenoid_close(solenoid_valve_t *valve);
result_t solenoid_get_state(solenoid_valve_t *valve, bool *is_open);

#endif
