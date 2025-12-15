/**
 * @file relay_output.h
 * @brief Unified Digital Output Driver
 *
 * Handles ALL digital outputs:
 * - Relay-controlled devices (pumps, solenoid valves, heaters)
 * - PWM outputs (variable speed pumps, dimmers)
 * - Simple GPIO outputs
 *
 * The output type is determined by configuration, not code.
 */

#ifndef RELAY_OUTPUT_H
#define RELAY_OUTPUT_H

#include "common.h"

/* ============================================================================
 * Output Types
 * ========================================================================== */

typedef enum {
    OUTPUT_TYPE_RELAY = 0,        // Simple on/off relay
    OUTPUT_TYPE_PWM,              // PWM variable output
    OUTPUT_TYPE_LATCHING,         // Latching relay (pulse to change state)
    OUTPUT_TYPE_MOMENTARY,        // Momentary contact (pulse output)
} output_type_t;

typedef enum {
    OUTPUT_STATE_OFF = 0,
    OUTPUT_STATE_ON = 1,
    OUTPUT_STATE_ERROR = -1,
    OUTPUT_STATE_UNKNOWN = -2,
} output_state_t;

/* ============================================================================
 * Output Configuration
 * ========================================================================== */

typedef struct {
    int id;
    char name[MAX_NAME_LEN];
    output_type_t type;

    // GPIO settings
    int gpio_pin;
    bool active_low;              // Inverted logic

    // PWM settings (if type == OUTPUT_TYPE_PWM)
    int pwm_frequency_hz;
    float pwm_min_duty;           // Minimum duty cycle (0.0 - 1.0)
    float pwm_max_duty;           // Maximum duty cycle

    // Timing
    int min_on_time_ms;           // Minimum on time (prevents rapid cycling)
    int min_off_time_ms;          // Minimum off time
    int pulse_duration_ms;        // For momentary/latching types

    // Safety
    int max_on_time_sec;          // Auto-shutoff after this time (0=disabled)
    bool interlock_group;         // Interlock with other outputs
    int interlock_id;             // Interlock group ID

} output_config_t;

/* ============================================================================
 * Output Status
 * ========================================================================== */

typedef struct {
    output_state_t state;
    float duty_cycle;             // 0.0-1.0 for PWM
    uint64_t last_change_ms;
    uint64_t total_on_time_ms;
    int cycle_count;
    bool locked_out;              // Due to timing or interlock
    char lockout_reason[64];
} output_status_t;

/* ============================================================================
 * Output Driver
 * ========================================================================== */

typedef struct output_driver output_driver_t;

struct output_driver {
    output_config_t config;
    output_status_t status;
    void *priv;
};

/* ============================================================================
 * Public API
 * ========================================================================== */

/**
 * Create an output driver
 */
result_t output_create(output_driver_t **drv, const output_config_t *cfg);
void output_destroy(output_driver_t *drv);

/**
 * Control output state
 */
result_t output_set(output_driver_t *drv, bool on);
result_t output_set_pwm(output_driver_t *drv, float duty_cycle);
result_t output_pulse(output_driver_t *drv, int duration_ms);
result_t output_toggle(output_driver_t *drv);

/**
 * Get status
 */
result_t output_get_state(output_driver_t *drv, output_state_t *state);
result_t output_get_status(output_driver_t *drv, output_status_t *status);

/**
 * Safety controls
 */
result_t output_emergency_stop(output_driver_t *drv);
result_t output_reset_lockout(output_driver_t *drv);

/**
 * Background processing (call periodically)
 * Handles auto-shutoff, timing constraints, etc.
 */
result_t output_process(output_driver_t *drv);

/**
 * Interlock management
 */
result_t output_set_interlock(output_driver_t *drv, int group_id);
result_t output_check_interlock(output_driver_t *drv, bool *allowed);

#endif /* RELAY_OUTPUT_H */
