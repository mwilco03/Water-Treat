/**
 * @file driver_pump.c
 * @brief Water pump driver
 *
 * @deprecated This driver is deprecated. Use relay_output.c with
 *             OUTPUT_TYPE_RELAY or OUTPUT_TYPE_PWM instead.
 *             Migration: src/drivers/digital/relay_output.c
 */
#warning "driver_pump.c is deprecated - use relay_output.c instead"

#include "driver_pump.h"
#include "utils/logger.h"
#include <fcntl.h>
#include <unistd.h>

result_t pump_init(water_pump_t *pump, int gpio_pin, bool use_pwm) {
    pump->use_pwm = use_pwm;
    pump->is_running = false;
    pump->period_ns = 1000000;  // 1ms = 1kHz
    pump->duty_ns = 0;
    pump->pwm_fd = -1;

    if (use_pwm) {
        // PWM initialization would go here
        // For now, fall back to GPIO
        LOG_WARNING("PWM not yet implemented, using GPIO on/off");
    }

    result_t result = hwif_gpio_export(&pump->gpio, gpio_pin);
    if (result != RESULT_OK) {
        return result;
    }

    hwif_gpio_set_direction(&pump->gpio, GPIO_DIR_OUT);
    hwif_gpio_write(&pump->gpio, false);  // Start off

    LOG_INFO("Initialized water pump on GPIO %d", gpio_pin);
    return RESULT_OK;
}

void pump_destroy(water_pump_t *pump) {
    pump_stop(pump);
    hwif_gpio_unexport(&pump->gpio);
}

result_t pump_start(water_pump_t *pump) {
    result_t result = hwif_gpio_write(&pump->gpio, true);

    if (result == RESULT_OK) {
        pump->is_running = true;
        LOG_INFO("Water pump started");
    }

    return result;
}

result_t pump_stop(water_pump_t *pump) {
    result_t result = hwif_gpio_write(&pump->gpio, false);

    if (result == RESULT_OK) {
        pump->is_running = false;
        LOG_INFO("Water pump stopped");
    }

    return result;
}

result_t pump_set_speed(water_pump_t *pump, uint8_t speed_percent) {
    if (speed_percent > 100) {
        speed_percent = 100;
    }

    if (!pump->use_pwm) {
        // Simple on/off
        if (speed_percent > 50) {
            return pump_start(pump);
        } else {
            return pump_stop(pump);
        }
    }

    // PWM implementation would go here
    pump->duty_ns = (pump->period_ns * speed_percent) / 100;

    LOG_DEBUG("Set pump speed to %d%%", speed_percent);
    return RESULT_OK;
}

result_t pump_get_state(water_pump_t *pump, bool *is_running) {
    *is_running = pump->is_running;
    return RESULT_OK;
}
