/**
 * @file l298n_driver.c
 * @brief L298N Dual H-Bridge Motor Driver Implementation
 *
 * Board-agnostic implementation using GPIO HAL.
 * Works on Raspberry Pi, Orange Pi, Luckfox, ODROID, and other supported SBCs.
 */

#include "l298n_driver.h"
#include "drivers/bus/gpio_hal.h"
#include "utils/logger.h"
#include <string.h>

/* ============================================================================
 * Internal Helpers
 * ========================================================================== */

/**
 * Apply direction control pins (IN1/IN2)
 */
static result_t apply_direction(l298n_motor_t *motor, l298n_direction_t dir) {
    bool in1_val, in2_val;

    switch (dir) {
        case L298N_DIR_FORWARD:
            in1_val = true;
            in2_val = false;
            break;
        case L298N_DIR_REVERSE:
            in1_val = false;
            in2_val = true;
            break;
        case L298N_DIR_BRAKE:
            in1_val = true;
            in2_val = true;
            break;
        case L298N_DIR_STOP:
        default:
            in1_val = false;
            in2_val = false;
            break;
    }

    /* Invert if active_low */
    if (motor->config.active_low) {
        in1_val = !in1_val;
        in2_val = !in2_val;
    }

    result_t res = gpio_write(motor->config.gpio_in1, in1_val);
    if (res != RESULT_OK) {
        LOG_ERROR("L298N [%s]: Failed to set IN1 (GPIO %d)",
                  motor->config.name, motor->config.gpio_in1);
        return res;
    }

    res = gpio_write(motor->config.gpio_in2, in2_val);
    if (res != RESULT_OK) {
        LOG_ERROR("L298N [%s]: Failed to set IN2 (GPIO %d)",
                  motor->config.name, motor->config.gpio_in2);
        return res;
    }

    return RESULT_OK;
}

/**
 * Apply PWM duty cycle to enable pin
 */
static result_t apply_pwm(l298n_motor_t *motor, uint8_t duty) {
    /* Clamp duty cycle to configured limits */
    if (duty > 0 && duty < motor->config.min_duty) {
        duty = motor->config.min_duty;
    }
    if (duty > motor->config.max_duty) {
        duty = motor->config.max_duty;
    }

    /* Check if hardware PWM is available */
    if (gpio_has_pwm(motor->config.gpio_enable)) {
        if (duty == 0) {
            return gpio_pwm_stop(motor->config.gpio_enable);
        }
        return gpio_pwm_set_duty(motor->config.gpio_enable, (float)duty);
    }

    /* Fallback: simple on/off (no speed control) */
    bool enable_val = (duty > 0);
    if (motor->config.active_low) {
        enable_val = !enable_val;
    }

    return gpio_write(motor->config.gpio_enable, enable_val);
}

/**
 * Update run time tracking
 */
static void update_run_time(l298n_motor_t *motor, bool now_running) {
    uint64_t now = get_time_ms();

    if (motor->is_running && !now_running) {
        /* Stopping - accumulate run time */
        motor->total_run_time_ms += (now - motor->start_time_ms);
    } else if (!motor->is_running && now_running) {
        /* Starting - record start time */
        motor->start_time_ms = now;
    }

    motor->is_running = now_running;
}

/* ============================================================================
 * Public API Implementation
 * ========================================================================== */

result_t l298n_init(l298n_motor_t *motor, const l298n_config_t *config) {
    CHECK_NULL(motor);
    CHECK_NULL(config);

    /* Validate configuration */
    result_t res = l298n_validate_config(config);
    if (res != RESULT_OK) {
        return res;
    }

    /* Initialize state */
    memset(motor, 0, sizeof(*motor));
    memcpy(&motor->config, config, sizeof(l298n_config_t));
    motor->direction = L298N_DIR_STOP;
    motor->duty_cycle = 0;
    motor->is_running = false;
    motor->is_initialized = false;

    LOG_INFO("L298N [%s]: Initializing motor driver", config->name);
    LOG_DEBUG("L298N [%s]: ENA=GPIO%d, IN1=GPIO%d, IN2=GPIO%d, PWM=%dHz",
              config->name, config->gpio_enable, config->gpio_in1,
              config->gpio_in2, config->pwm_frequency_hz);

    /* Configure direction pins as outputs */
    res = gpio_configure(config->gpio_in1, GPIO_DIR_OUTPUT, GPIO_PULL_NONE);
    if (res != RESULT_OK) {
        LOG_ERROR("L298N [%s]: Failed to configure IN1 (GPIO %d): %s",
                  config->name, config->gpio_in1, result_to_string(res));
        return res;
    }

    res = gpio_configure(config->gpio_in2, GPIO_DIR_OUTPUT, GPIO_PULL_NONE);
    if (res != RESULT_OK) {
        LOG_ERROR("L298N [%s]: Failed to configure IN2 (GPIO %d): %s",
                  config->name, config->gpio_in2, result_to_string(res));
        return res;
    }

    /* Configure enable pin - try PWM first, fall back to GPIO */
    if (gpio_has_pwm(config->gpio_enable)) {
        LOG_INFO("L298N [%s]: Using hardware PWM on GPIO %d @ %d Hz",
                 config->name, config->gpio_enable, config->pwm_frequency_hz);
        res = gpio_pwm_start(config->gpio_enable, config->pwm_frequency_hz, 0.0f);
        if (res != RESULT_OK) {
            LOG_WARNING("L298N [%s]: PWM init failed, falling back to GPIO on/off",
                        config->name);
            res = gpio_configure(config->gpio_enable, GPIO_DIR_OUTPUT, GPIO_PULL_NONE);
        }
    } else {
        LOG_WARNING("L298N [%s]: GPIO %d has no PWM - using on/off control only",
                    config->name, config->gpio_enable);
        res = gpio_configure(config->gpio_enable, GPIO_DIR_OUTPUT, GPIO_PULL_NONE);
    }

    if (res != RESULT_OK) {
        LOG_ERROR("L298N [%s]: Failed to configure enable pin (GPIO %d)",
                  config->name, config->gpio_enable);
        return res;
    }

    /* Set initial state: stopped */
    apply_direction(motor, L298N_DIR_STOP);
    apply_pwm(motor, 0);

    motor->is_initialized = true;
    LOG_INFO("L298N [%s]: Motor driver initialized successfully", config->name);

    return RESULT_OK;
}

void l298n_destroy(l298n_motor_t *motor) {
    if (!motor || !motor->is_initialized) {
        return;
    }

    LOG_INFO("L298N [%s]: Shutting down motor driver", motor->config.name);

    /* Stop motor first */
    l298n_stop(motor);

    /* Stop PWM if active */
    if (gpio_has_pwm(motor->config.gpio_enable)) {
        gpio_pwm_stop(motor->config.gpio_enable);
    }

    motor->is_initialized = false;

    LOG_DEBUG("L298N [%s]: Total run time: %lu ms",
              motor->config.name, (unsigned long)motor->total_run_time_ms);
}

result_t l298n_set_speed(l298n_motor_t *motor, uint8_t speed_percent) {
    return l298n_set_direction(motor, L298N_DIR_FORWARD, speed_percent);
}

result_t l298n_set_direction(l298n_motor_t *motor, l298n_direction_t direction,
                             uint8_t speed_percent) {
    CHECK_NULL(motor);

    if (!motor->is_initialized) {
        LOG_ERROR("L298N: Motor not initialized");
        return RESULT_NOT_INITIALIZED;
    }

    /* Check if reverse is allowed */
    if (direction == L298N_DIR_REVERSE && !motor->config.enable_reverse) {
        LOG_WARNING("L298N [%s]: Reverse direction not enabled, ignoring",
                    motor->config.name);
        return RESULT_NOT_SUPPORTED;
    }

    /* Clamp speed */
    if (speed_percent > 100) {
        speed_percent = 100;
    }

    /* For stop/brake, speed is always 0 */
    if (direction == L298N_DIR_STOP || direction == L298N_DIR_BRAKE) {
        speed_percent = 0;
    }

    /* Apply direction control */
    result_t res = apply_direction(motor, direction);
    if (res != RESULT_OK) {
        return res;
    }

    /* Apply speed (PWM) */
    res = apply_pwm(motor, speed_percent);
    if (res != RESULT_OK) {
        return res;
    }

    /* Update state */
    motor->direction = direction;
    motor->duty_cycle = speed_percent;
    update_run_time(motor, speed_percent > 0);

    LOG_DEBUG("L298N [%s]: %s @ %d%%",
              motor->config.name, l298n_direction_to_string(direction),
              speed_percent);

    return RESULT_OK;
}

result_t l298n_stop(l298n_motor_t *motor) {
    return l298n_set_direction(motor, L298N_DIR_STOP, 0);
}

result_t l298n_brake(l298n_motor_t *motor) {
    CHECK_NULL(motor);

    if (!motor->is_initialized) {
        return RESULT_NOT_INITIALIZED;
    }

    /* For brake: set IN1=H, IN2=H, enable=H */
    result_t res = apply_direction(motor, L298N_DIR_BRAKE);
    if (res != RESULT_OK) {
        return res;
    }

    /* Enable at full for braking */
    res = apply_pwm(motor, 100);
    if (res != RESULT_OK) {
        return res;
    }

    motor->direction = L298N_DIR_BRAKE;
    motor->duty_cycle = 0;
    update_run_time(motor, false);

    LOG_DEBUG("L298N [%s]: Brake applied", motor->config.name);

    return RESULT_OK;
}

result_t l298n_get_status(const l298n_motor_t *motor, l298n_status_t *status) {
    CHECK_NULL(motor);
    CHECK_NULL(status);

    status->initialized = motor->is_initialized;
    status->running = motor->is_running;
    status->direction = motor->direction;
    status->duty_cycle = motor->duty_cycle;
    status->total_run_time_ms = motor->total_run_time_ms;

    if (motor->is_running) {
        status->run_time_ms = get_time_ms() - motor->start_time_ms;
    } else {
        status->run_time_ms = 0;
    }

    return RESULT_OK;
}

bool l298n_is_running(const l298n_motor_t *motor) {
    return motor && motor->is_initialized && motor->is_running;
}

uint8_t l298n_get_duty(const l298n_motor_t *motor) {
    if (!motor || !motor->is_initialized) {
        return 0;
    }
    return motor->duty_cycle;
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

const char* l298n_direction_to_string(l298n_direction_t dir) {
    switch (dir) {
        case L298N_DIR_STOP:    return "STOP";
        case L298N_DIR_FORWARD: return "FORWARD";
        case L298N_DIR_REVERSE: return "REVERSE";
        case L298N_DIR_BRAKE:   return "BRAKE";
        default:                return "UNKNOWN";
    }
}

result_t l298n_validate_config(const l298n_config_t *config) {
    CHECK_NULL(config);

    if (config->gpio_enable < 0) {
        LOG_ERROR("L298N: Invalid enable pin (%d)", config->gpio_enable);
        return RESULT_INVALID_PARAM;
    }

    if (config->gpio_in1 < 0) {
        LOG_ERROR("L298N: Invalid IN1 pin (%d)", config->gpio_in1);
        return RESULT_INVALID_PARAM;
    }

    if (config->gpio_in2 < 0) {
        LOG_ERROR("L298N: Invalid IN2 pin (%d)", config->gpio_in2);
        return RESULT_INVALID_PARAM;
    }

    /* Check for pin conflicts */
    if (config->gpio_enable == config->gpio_in1 ||
        config->gpio_enable == config->gpio_in2 ||
        config->gpio_in1 == config->gpio_in2) {
        LOG_ERROR("L298N: Pin conflict detected (ENA=%d, IN1=%d, IN2=%d)",
                  config->gpio_enable, config->gpio_in1, config->gpio_in2);
        return RESULT_INVALID_PARAM;
    }

    if (config->pwm_frequency_hz < L298N_MIN_PWM_FREQ_HZ ||
        config->pwm_frequency_hz > L298N_MAX_PWM_FREQ_HZ) {
        LOG_ERROR("L298N: Invalid PWM frequency (%d Hz, valid: %d-%d)",
                  config->pwm_frequency_hz,
                  L298N_MIN_PWM_FREQ_HZ, L298N_MAX_PWM_FREQ_HZ);
        return RESULT_INVALID_PARAM;
    }

    if (config->min_duty > config->max_duty) {
        LOG_ERROR("L298N: min_duty (%d) > max_duty (%d)",
                  config->min_duty, config->max_duty);
        return RESULT_INVALID_PARAM;
    }

    if (config->max_duty > 100) {
        LOG_ERROR("L298N: max_duty (%d) exceeds 100%%", config->max_duty);
        return RESULT_INVALID_PARAM;
    }

    return RESULT_OK;
}
