/**
 * @file gpio_hal.h
 * @brief GPIO Hardware Abstraction Layer
 *
 * Platform-independent GPIO interface.
 * Supports Linux sysfs, gpiod, and direct register access.
 */

#ifndef GPIO_HAL_H
#define GPIO_HAL_H

#include "common.h"

/* ============================================================================
 * GPIO Configuration
 * ========================================================================== */

typedef enum {
    GPIO_DIR_INPUT = 0,
    GPIO_DIR_OUTPUT,
} gpio_direction_t;

typedef enum {
    GPIO_PULL_NONE = 0,
    GPIO_PULL_UP,
    GPIO_PULL_DOWN,
} gpio_pull_t;

typedef enum {
    GPIO_EDGE_NONE = 0,
    GPIO_EDGE_RISING,
    GPIO_EDGE_FALLING,
    GPIO_EDGE_BOTH,
} gpio_edge_t;

/* ============================================================================
 * GPIO Functions
 * ========================================================================== */

/**
 * Initialize GPIO subsystem
 */
result_t gpio_init(void);
void gpio_shutdown(void);

/**
 * Configure a GPIO pin
 */
result_t gpio_configure(int pin, gpio_direction_t dir, gpio_pull_t pull);

/**
 * Read/Write GPIO
 */
result_t gpio_read(int pin, bool *value);
result_t gpio_write(int pin, bool value);

/**
 * Edge detection (for interrupt-driven input)
 */
typedef void (*gpio_callback_t)(int pin, bool value, void *ctx);

result_t gpio_set_edge(int pin, gpio_edge_t edge);
result_t gpio_wait_edge(int pin, int timeout_ms, bool *value);
result_t gpio_add_callback(int pin, gpio_callback_t callback, void *ctx);
result_t gpio_remove_callback(int pin);

/**
 * PWM (if supported)
 */
bool gpio_has_pwm(int pin);
result_t gpio_pwm_start(int pin, int frequency_hz, float duty_cycle);
result_t gpio_pwm_set_duty(int pin, float duty_cycle);
result_t gpio_pwm_stop(int pin);

#endif /* GPIO_HAL_H */
