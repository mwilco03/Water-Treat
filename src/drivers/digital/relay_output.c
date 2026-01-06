/**
 * @file relay_output.c
 * @brief Unified Digital Output Driver Implementation
 *
 * Handles pumps, solenoids, relays, and PWM outputs.
 * All controlled by configuration.
 */

#include "relay_output.h"
#include "drivers/bus/gpio_hal.h"
#include "utils/logger.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ============================================================================
 * Private Data
 * ========================================================================== */

typedef struct {
    bool gpio_initialized;
    uint64_t on_start_time;
    uint64_t off_start_time;
} output_priv_t;

/* Interlock tracking - mutex protected for thread safety (safety-critical) */
#define MAX_INTERLOCKS 16
static struct {
    int group_id;
    output_driver_t *active_output;
} g_interlocks[MAX_INTERLOCKS];
static int g_interlock_count = 0;
static pthread_mutex_t g_interlock_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================================
 * GPIO Operations (abstracted for portability)
 * ========================================================================== */

static result_t gpio_set_output(int pin, bool value) {
#ifdef __linux__
    // Linux sysfs GPIO or gpiod
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);

    FILE *f = fopen(path, "w");
    if (!f) {
        // Try to export first
        FILE *exp = fopen("/sys/class/gpio/export", "w");
        if (exp) {
            fprintf(exp, "%d", pin);
            fclose(exp);

            // Set direction
            snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
            FILE *dir = fopen(path, "w");
            if (dir) {
                fprintf(dir, "out");
                fclose(dir);
            }

            // Try again
            snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
            f = fopen(path, "w");
        }
    }

    if (f) {
        fprintf(f, "%d", value ? 1 : 0);
        fclose(f);
        return RESULT_OK;
    }

    LOG_WARNING("Failed to set GPIO %d", pin);
    return RESULT_IO_ERROR;
#else
    UNUSED(pin);
    UNUSED(value);
    LOG_DEBUG("GPIO simulation: pin %d = %d", pin, value);
    return RESULT_OK;
#endif
}

static result_t gpio_set_pwm(int pin, float duty_cycle, int frequency_hz) {
    // PWM is platform-specific
    // On Raspberry Pi, use hardware PWM or pigpio
    // This is a placeholder that falls back to on/off

    UNUSED(frequency_hz);

    if (duty_cycle > 0.5f) {
        return gpio_set_output(pin, true);
    } else {
        return gpio_set_output(pin, false);
    }
}

/* ============================================================================
 * Interlock Management (thread-safe - all access protected by g_interlock_mutex)
 *
 * SAFETY CRITICAL: These functions manage mutual exclusion between actuators
 * in the same interlock group. Race conditions here could allow multiple
 * conflicting actuators to activate simultaneously.
 * ========================================================================== */

static bool check_interlock_available(int group_id, output_driver_t *drv) {
    pthread_mutex_lock(&g_interlock_mutex);
    bool available = true;
    for (int i = 0; i < g_interlock_count; i++) {
        if (g_interlocks[i].group_id == group_id &&
            g_interlocks[i].active_output != NULL &&
            g_interlocks[i].active_output != drv) {
            available = false;  // Another output in this group is active
            break;
        }
    }
    pthread_mutex_unlock(&g_interlock_mutex);
    return available;
}

static void register_interlock_active(int group_id, output_driver_t *drv) {
    pthread_mutex_lock(&g_interlock_mutex);
    for (int i = 0; i < g_interlock_count; i++) {
        if (g_interlocks[i].group_id == group_id) {
            g_interlocks[i].active_output = drv;
            pthread_mutex_unlock(&g_interlock_mutex);
            LOG_DEBUG("Interlock group %d: registered active output '%s'",
                      group_id, drv->config.name);
            return;
        }
    }

    // New group
    if (g_interlock_count < MAX_INTERLOCKS) {
        g_interlocks[g_interlock_count].group_id = group_id;
        g_interlocks[g_interlock_count].active_output = drv;
        g_interlock_count++;
        LOG_DEBUG("Interlock group %d: created and registered active output '%s'",
                  group_id, drv->config.name);
    } else {
        LOG_ERROR("SAFETY WARNING: Max interlock groups (%d) exceeded, cannot register group %d",
                  MAX_INTERLOCKS, group_id);
    }
    pthread_mutex_unlock(&g_interlock_mutex);
}

static void clear_interlock_active(int group_id, output_driver_t *drv) {
    pthread_mutex_lock(&g_interlock_mutex);
    for (int i = 0; i < g_interlock_count; i++) {
        if (g_interlocks[i].group_id == group_id &&
            g_interlocks[i].active_output == drv) {
            g_interlocks[i].active_output = NULL;
            LOG_DEBUG("Interlock group %d: cleared active output '%s'",
                      group_id, drv->config.name);
        }
    }
    pthread_mutex_unlock(&g_interlock_mutex);
}

/* ============================================================================
 * Public API
 * ========================================================================== */

result_t output_create(output_driver_t **drv, const output_config_t *cfg) {
    CHECK_NULL(drv);
    CHECK_NULL(cfg);

    output_driver_t *d = calloc(1, sizeof(output_driver_t));
    if (!d) return RESULT_NO_MEMORY;

    output_priv_t *priv = calloc(1, sizeof(output_priv_t));
    if (!priv) {
        free(d);
        return RESULT_NO_MEMORY;
    }

    memcpy(&d->config, cfg, sizeof(output_config_t));
    d->priv = priv;
    d->status.state = OUTPUT_STATE_OFF;
    d->status.last_change_ms = get_time_ms();

    // Initialize GPIO
    result_t r = gpio_set_output(cfg->gpio_pin, cfg->active_low ? 1 : 0);
    if (r == RESULT_OK) {
        priv->gpio_initialized = true;
    }

    LOG_INFO("Output '%s' created on GPIO %d (active_low=%d)",
             cfg->name, cfg->gpio_pin, cfg->active_low);

    *drv = d;
    return RESULT_OK;
}

void output_destroy(output_driver_t *drv) {
    if (!drv) return;

    // Turn off before destroying
    output_set(drv, false);

    if (drv->config.interlock_group) {
        clear_interlock_active(drv->config.interlock_id, drv);
    }

    if (drv->priv) {
        free(drv->priv);
    }
    free(drv);
}

result_t output_set(output_driver_t *drv, bool on) {
    CHECK_NULL(drv);

    output_priv_t *priv = drv->priv;
    uint64_t now = get_time_ms();

    // Check timing constraints
    if (on && drv->config.min_off_time_ms > 0) {
        if ((now - priv->off_start_time) < (uint64_t)drv->config.min_off_time_ms) {
            drv->status.locked_out = true;
            SAFE_STRNCPY(drv->status.lockout_reason, "Min off time",
                         sizeof(drv->status.lockout_reason));
            return RESULT_BUSY;
        }
    }

    if (!on && drv->config.min_on_time_ms > 0) {
        if ((now - priv->on_start_time) < (uint64_t)drv->config.min_on_time_ms) {
            drv->status.locked_out = true;
            SAFE_STRNCPY(drv->status.lockout_reason, "Min on time",
                         sizeof(drv->status.lockout_reason));
            return RESULT_BUSY;
        }
    }

    // Check interlock
    if (on && drv->config.interlock_group) {
        if (!check_interlock_available(drv->config.interlock_id, drv)) {
            drv->status.locked_out = true;
            SAFE_STRNCPY(drv->status.lockout_reason, "Interlock active",
                         sizeof(drv->status.lockout_reason));
            return RESULT_BUSY;
        }
    }

    // Apply output
    bool gpio_value = on;
    if (drv->config.active_low) {
        gpio_value = !gpio_value;
    }

    result_t r = gpio_set_output(drv->config.gpio_pin, gpio_value);
    if (r != RESULT_OK) {
        drv->status.state = OUTPUT_STATE_ERROR;
        return r;
    }

    // Update status
    output_state_t old_state = drv->status.state;
    drv->status.state = on ? OUTPUT_STATE_ON : OUTPUT_STATE_OFF;
    drv->status.locked_out = false;
    drv->status.lockout_reason[0] = '\0';

    if (old_state != drv->status.state) {
        drv->status.last_change_ms = now;
        drv->status.cycle_count++;

        if (on) {
            priv->on_start_time = now;
            if (drv->config.interlock_group) {
                register_interlock_active(drv->config.interlock_id, drv);
            }
        } else {
            priv->off_start_time = now;
            // Accumulate on time
            if (priv->on_start_time > 0) {
                drv->status.total_on_time_ms += (now - priv->on_start_time);
            }
            if (drv->config.interlock_group) {
                clear_interlock_active(drv->config.interlock_id, drv);
            }
        }

        LOG_DEBUG("Output '%s' %s", drv->config.name, on ? "ON" : "OFF");
    }

    return RESULT_OK;
}

result_t output_set_pwm(output_driver_t *drv, float duty_cycle) {
    CHECK_NULL(drv);

    if (drv->config.type != OUTPUT_TYPE_PWM) {
        // Fall back to on/off
        return output_set(drv, duty_cycle > 0.5f);
    }

    // Clamp duty cycle
    if (duty_cycle < drv->config.pwm_min_duty) {
        duty_cycle = drv->config.pwm_min_duty;
    }
    if (duty_cycle > drv->config.pwm_max_duty) {
        duty_cycle = drv->config.pwm_max_duty;
    }

    result_t r = gpio_set_pwm(drv->config.gpio_pin, duty_cycle,
                              drv->config.pwm_frequency_hz);

    if (r == RESULT_OK) {
        drv->status.duty_cycle = duty_cycle;
        drv->status.state = (duty_cycle > 0.01f) ? OUTPUT_STATE_ON : OUTPUT_STATE_OFF;
    }

    return r;
}

result_t output_pulse(output_driver_t *drv, int duration_ms) {
    CHECK_NULL(drv);

    if (duration_ms <= 0) {
        duration_ms = drv->config.pulse_duration_ms;
    }
    if (duration_ms <= 0) {
        duration_ms = 100;  // Default pulse
    }

    result_t r = output_set(drv, true);
    if (r != RESULT_OK) return r;

    // In a real implementation, this would use a timer
    // For now, blocking delay
    struct timespec ts = {
        .tv_sec = duration_ms / 1000,
        .tv_nsec = (duration_ms % 1000) * 1000000
    };
    nanosleep(&ts, NULL);

    return output_set(drv, false);
}

result_t output_toggle(output_driver_t *drv) {
    CHECK_NULL(drv);
    return output_set(drv, drv->status.state != OUTPUT_STATE_ON);
}

result_t output_get_state(output_driver_t *drv, output_state_t *state) {
    CHECK_NULL(drv);
    CHECK_NULL(state);
    *state = drv->status.state;
    return RESULT_OK;
}

result_t output_get_status(output_driver_t *drv, output_status_t *status) {
    CHECK_NULL(drv);
    CHECK_NULL(status);
    memcpy(status, &drv->status, sizeof(output_status_t));
    return RESULT_OK;
}

result_t output_emergency_stop(output_driver_t *drv) {
    CHECK_NULL(drv);

    // Force off immediately, bypass timing constraints
    bool gpio_value = drv->config.active_low ? 1 : 0;
    gpio_set_output(drv->config.gpio_pin, gpio_value);

    drv->status.state = OUTPUT_STATE_OFF;
    drv->status.locked_out = true;
    SAFE_STRNCPY(drv->status.lockout_reason, "Emergency stop",
                 sizeof(drv->status.lockout_reason));

    LOG_WARNING("Output '%s' EMERGENCY STOP", drv->config.name);

    return RESULT_OK;
}

result_t output_reset_lockout(output_driver_t *drv) {
    CHECK_NULL(drv);
    drv->status.locked_out = false;
    drv->status.lockout_reason[0] = '\0';
    return RESULT_OK;
}

result_t output_process(output_driver_t *drv) {
    CHECK_NULL(drv);

    // Check auto-shutoff
    if (drv->config.max_on_time_sec > 0 &&
        drv->status.state == OUTPUT_STATE_ON) {

        output_priv_t *priv = drv->priv;
        uint64_t on_duration = get_time_ms() - priv->on_start_time;

        if (on_duration > (uint64_t)drv->config.max_on_time_sec * 1000) {
            LOG_WARNING("Output '%s' auto-shutoff after %d seconds",
                        drv->config.name, drv->config.max_on_time_sec);
            output_set(drv, false);
            drv->status.locked_out = true;
            SAFE_STRNCPY(drv->status.lockout_reason, "Auto shutoff",
                         sizeof(drv->status.lockout_reason));
        }
    }

    return RESULT_OK;
}

result_t output_check_interlock(output_driver_t *drv, bool *allowed) {
    CHECK_NULL(drv);
    CHECK_NULL(allowed);

    if (!drv->config.interlock_group) {
        *allowed = true;
        return RESULT_OK;
    }

    *allowed = check_interlock_available(drv->config.interlock_id, drv);
    return RESULT_OK;
}
