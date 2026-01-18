/**
 * @file dosing_pump.c
 * @brief Dosing Pump Control Implementation
 *
 * High-level dosing pump control using L298N motor driver.
 * Provides board-agnostic initialization via automatic detection.
 */

#include "dosing_pump.h"
#include "platform/board_detect.h"
#include "utils/logger.h"
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Initialization
 * ========================================================================== */

result_t dosing_pump_init_auto(dosing_pump_t *pump, const char *name) {
    CHECK_NULL(pump);

    /* Detect board and get pin configuration */
    board_info_t board;
    result_t res = board_detect(&board);
    if (res != RESULT_OK) {
        LOG_ERROR("DosingPump: Failed to detect board");
        return res;
    }

    /* Check if motor pins are configured for this board */
    if (board.pins.motor_a_enable < 0 ||
        board.pins.motor_a_in1 < 0 ||
        board.pins.motor_a_in2 < 0) {
        LOG_ERROR("DosingPump: Board '%s' has no motor driver pins configured",
                  board.name);
        return RESULT_NOT_SUPPORTED;
    }

    LOG_INFO("DosingPump: Auto-detected board '%s'", board.name);
    LOG_INFO("DosingPump: Using pins ENA=%d, IN1=%d, IN2=%d",
             board.pins.motor_a_enable,
             board.pins.motor_a_in1,
             board.pins.motor_a_in2);

    /* Build configuration from detected pins */
    dosing_pump_config_t cfg = dosing_pump_default_config();
    cfg.gpio_enable = board.pins.motor_a_enable;
    cfg.gpio_in1 = board.pins.motor_a_in1;
    cfg.gpio_in2 = board.pins.motor_a_in2;

    if (name && name[0]) {
        SAFE_STRNCPY(cfg.name, name, sizeof(cfg.name));
    }

    return dosing_pump_init(pump, &cfg);
}

result_t dosing_pump_init(dosing_pump_t *pump, const dosing_pump_config_t *config) {
    CHECK_NULL(pump);
    CHECK_NULL(config);

    memset(pump, 0, sizeof(*pump));
    memcpy(&pump->config, config, sizeof(dosing_pump_config_t));

    LOG_INFO("DosingPump [%s]: Initializing", config->name);

    /* Build L298N motor configuration */
    l298n_config_t motor_cfg = l298n_default_config();
    motor_cfg.gpio_enable = config->gpio_enable;
    motor_cfg.gpio_in1 = config->gpio_in1;
    motor_cfg.gpio_in2 = config->gpio_in2;
    motor_cfg.pwm_frequency_hz = config->pwm_frequency_hz;
    motor_cfg.min_duty = config->min_duty;
    motor_cfg.max_duty = config->max_duty;
    motor_cfg.enable_reverse = false;  /* Dosing pumps are unidirectional */
    SAFE_STRNCPY(motor_cfg.name, config->name, sizeof(motor_cfg.name));

    /* Initialize underlying motor driver */
    result_t res = l298n_init(&pump->motor, &motor_cfg);
    if (res != RESULT_OK) {
        LOG_ERROR("DosingPump [%s]: Failed to initialize motor driver",
                  config->name);
        return res;
    }

    pump->total_ml_dispensed = 0.0f;
    pump->is_initialized = true;

    LOG_INFO("DosingPump [%s]: Initialized successfully (%.1f mL/min @ 100%%)",
             config->name, config->ml_per_min_at_100);

    return RESULT_OK;
}

void dosing_pump_destroy(dosing_pump_t *pump) {
    if (!pump || !pump->is_initialized) {
        return;
    }

    LOG_INFO("DosingPump [%s]: Shutting down (total dispensed: %.1f mL)",
             pump->config.name, pump->total_ml_dispensed);

    l298n_destroy(&pump->motor);
    pump->is_initialized = false;
}

/* ============================================================================
 * Pump Control
 * ========================================================================== */

result_t dosing_pump_set_rate(dosing_pump_t *pump, uint8_t percent) {
    CHECK_NULL(pump);

    if (!pump->is_initialized) {
        return RESULT_NOT_INITIALIZED;
    }

    if (percent == 0) {
        return l298n_stop(&pump->motor);
    }

    return l298n_set_speed(&pump->motor, percent);
}

result_t dosing_pump_dose_ml(dosing_pump_t *pump, float ml, uint8_t rate_percent) {
    CHECK_NULL(pump);

    if (!pump->is_initialized) {
        return RESULT_NOT_INITIALIZED;
    }

    if (ml <= 0.0f) {
        LOG_WARNING("DosingPump [%s]: Invalid volume %.2f mL",
                    pump->config.name, ml);
        return RESULT_INVALID_PARAM;
    }

    if (rate_percent < pump->config.min_duty || rate_percent > 100) {
        LOG_WARNING("DosingPump [%s]: Rate %d%% out of range (%d-%d)",
                    pump->config.name, rate_percent, pump->config.min_duty, 100);
        return RESULT_INVALID_PARAM;
    }

    /* Calculate dosing time based on calibration */
    float flow_rate = pump->config.ml_per_min_at_100 * (rate_percent / 100.0f);
    float dose_time_sec = (ml / flow_rate) * 60.0f;
    uint32_t dose_time_ms = (uint32_t)(dose_time_sec * 1000.0f);

    LOG_INFO("DosingPump [%s]: Dosing %.2f mL @ %d%% (%.1f sec)",
             pump->config.name, ml, rate_percent, dose_time_sec);

    /* Start pump */
    result_t res = l298n_set_speed(&pump->motor, rate_percent);
    if (res != RESULT_OK) {
        return res;
    }

    /* Wait for dose completion */
    usleep(dose_time_ms * 1000);

    /* Stop pump */
    l298n_stop(&pump->motor);

    /* Update counter */
    pump->total_ml_dispensed += ml;

    LOG_DEBUG("DosingPump [%s]: Dose complete (total: %.1f mL)",
              pump->config.name, pump->total_ml_dispensed);

    return RESULT_OK;
}

result_t dosing_pump_start(dosing_pump_t *pump, uint8_t rate_percent) {
    CHECK_NULL(pump);

    if (!pump->is_initialized) {
        return RESULT_NOT_INITIALIZED;
    }

    if (rate_percent < 1 || rate_percent > 100) {
        return RESULT_INVALID_PARAM;
    }

    LOG_DEBUG("DosingPump [%s]: Starting continuous @ %d%%",
              pump->config.name, rate_percent);

    return l298n_set_speed(&pump->motor, rate_percent);
}

result_t dosing_pump_stop(dosing_pump_t *pump) {
    CHECK_NULL(pump);

    if (!pump->is_initialized) {
        return RESULT_NOT_INITIALIZED;
    }

    LOG_DEBUG("DosingPump [%s]: Stopping", pump->config.name);

    return l298n_stop(&pump->motor);
}

result_t dosing_pump_prime(dosing_pump_t *pump, uint32_t duration_ms) {
    CHECK_NULL(pump);

    if (!pump->is_initialized) {
        return RESULT_NOT_INITIALIZED;
    }

    if (duration_ms == 0) {
        duration_ms = 3000;  /* Default 3 seconds */
    }

    LOG_INFO("DosingPump [%s]: Priming for %u ms", pump->config.name, duration_ms);

    /* Run at 80% for priming (not 100% to reduce stress) */
    result_t res = l298n_set_speed(&pump->motor, 80);
    if (res != RESULT_OK) {
        return res;
    }

    usleep(duration_ms * 1000);

    l298n_stop(&pump->motor);

    LOG_INFO("DosingPump [%s]: Prime complete", pump->config.name);

    return RESULT_OK;
}

/* ============================================================================
 * Status & Calibration
 * ========================================================================== */

result_t dosing_pump_get_status(const dosing_pump_t *pump, dosing_pump_status_t *status) {
    CHECK_NULL(pump);
    CHECK_NULL(status);

    memset(status, 0, sizeof(*status));

    status->initialized = pump->is_initialized;
    status->total_ml_dispensed = pump->total_ml_dispensed;

    if (pump->is_initialized) {
        l298n_status_t motor_status;
        l298n_get_status(&pump->motor, &motor_status);

        status->running = motor_status.running;
        status->duty_cycle = motor_status.duty_cycle;
        status->run_time_ms = motor_status.run_time_ms;

        /* Calculate current flow rate */
        if (motor_status.running && motor_status.duty_cycle > 0) {
            status->current_flow_ml_min =
                pump->config.ml_per_min_at_100 * (motor_status.duty_cycle / 100.0f);
        }
    }

    return RESULT_OK;
}

bool dosing_pump_is_running(const dosing_pump_t *pump) {
    if (!pump || !pump->is_initialized) {
        return false;
    }
    return l298n_is_running(&pump->motor);
}

result_t dosing_pump_set_calibration(dosing_pump_t *pump, float ml_per_min) {
    CHECK_NULL(pump);

    if (ml_per_min <= 0.0f || ml_per_min > 10000.0f) {
        LOG_ERROR("DosingPump: Invalid calibration %.2f mL/min", ml_per_min);
        return RESULT_INVALID_PARAM;
    }

    pump->config.ml_per_min_at_100 = ml_per_min;
    LOG_INFO("DosingPump [%s]: Calibration set to %.1f mL/min @ 100%%",
             pump->config.name, ml_per_min);

    return RESULT_OK;
}

void dosing_pump_reset_counter(dosing_pump_t *pump) {
    if (pump) {
        pump->total_ml_dispensed = 0.0f;
        LOG_DEBUG("DosingPump [%s]: Counter reset", pump->config.name);
    }
}
