/**
 * @file l298n_driver.h
 * @brief L298N Dual H-Bridge Motor Driver
 *
 * Board-agnostic driver for L298N motor controller module.
 * Supports PWM speed control and bi-directional motor operation.
 *
 * Hardware connections (per motor channel):
 *   - ENA/ENB: PWM pin for speed control (remove jumper cap)
 *   - IN1/IN3: Direction control pin 1
 *   - IN2/IN4: Direction control pin 2
 *
 * Control logic:
 *   | ENA | IN1 | IN2 | Motor State        |
 *   |-----|-----|-----|---------------------|
 *   | PWM | H   | L   | Forward @ duty%     |
 *   | PWM | L   | H   | Reverse @ duty%     |
 *   | PWM | L   | L   | Coast (free spin)   |
 *   | PWM | H   | H   | Brake (hard stop)   |
 *
 * Usage:
 *   l298n_motor_t motor;
 *   l298n_init(&motor, &config);
 *   l298n_set_speed(&motor, 75);    // 75% forward
 *   l298n_stop(&motor);
 *   l298n_destroy(&motor);
 */

#ifndef L298N_DRIVER_H
#define L298N_DRIVER_H

#include "common.h"

/* ============================================================================
 * Configuration
 * ========================================================================== */

/** Maximum number of L298N motor instances */
#define L298N_MAX_MOTORS 4

/** Default PWM frequency for motor control (Hz) */
#define L298N_DEFAULT_PWM_FREQ_HZ 1000

/** Minimum PWM frequency (Hz) */
#define L298N_MIN_PWM_FREQ_HZ 100

/** Maximum PWM frequency (Hz) */
#define L298N_MAX_PWM_FREQ_HZ 25000

/* ============================================================================
 * Types
 * ========================================================================== */

/**
 * Motor direction enumeration
 */
typedef enum {
    L298N_DIR_STOP = 0,     /**< Motor stopped (coast) */
    L298N_DIR_FORWARD,      /**< Forward direction (IN1=H, IN2=L) */
    L298N_DIR_REVERSE,      /**< Reverse direction (IN1=L, IN2=H) */
    L298N_DIR_BRAKE         /**< Active brake (IN1=H, IN2=H) */
} l298n_direction_t;

/**
 * Motor channel enumeration (L298N has 2 channels)
 */
typedef enum {
    L298N_CHANNEL_A = 0,    /**< Motor A (ENA, IN1, IN2) */
    L298N_CHANNEL_B = 1     /**< Motor B (ENB, IN3, IN4) */
} l298n_channel_t;

/**
 * Motor configuration structure
 *
 * Pin numbers are chip-relative GPIO numbers (board-agnostic).
 * Use board_detect to get appropriate pin numbers for your SBC.
 */
typedef struct {
    int gpio_enable;        /**< Enable pin (ENA/ENB) - PWM capable */
    int gpio_in1;           /**< Direction pin 1 (IN1/IN3) */
    int gpio_in2;           /**< Direction pin 2 (IN2/IN4) */
    int pwm_frequency_hz;   /**< PWM frequency (default: 1000 Hz) */
    bool active_low;        /**< Invert logic if needed */
    bool enable_reverse;    /**< Allow reverse direction */
    uint8_t min_duty;       /**< Minimum duty cycle % (motor stall threshold) */
    uint8_t max_duty;       /**< Maximum duty cycle % (safety limit) */
    char name[MAX_NAME_LEN]; /**< Human-readable motor name */
} l298n_config_t;

/**
 * Motor runtime state
 */
typedef struct {
    l298n_config_t config;      /**< Configuration (copy) */
    l298n_direction_t direction; /**< Current direction */
    uint8_t duty_cycle;         /**< Current duty cycle (0-100) */
    bool is_running;            /**< Motor is active */
    bool is_initialized;        /**< Driver initialized successfully */
    uint64_t start_time_ms;     /**< Time motor was started */
    uint64_t total_run_time_ms; /**< Cumulative run time */
} l298n_motor_t;

/**
 * Motor status for diagnostics
 */
typedef struct {
    bool initialized;
    bool running;
    l298n_direction_t direction;
    uint8_t duty_cycle;
    uint64_t run_time_ms;
    uint64_t total_run_time_ms;
} l298n_status_t;

/* ============================================================================
 * Default Configuration
 * ========================================================================== */

/**
 * Get default motor configuration
 *
 * Provides sensible defaults:
 *   - PWM frequency: 1000 Hz
 *   - Min duty: 0%
 *   - Max duty: 100%
 *   - Reverse: disabled (forward only)
 *   - Active low: false
 */
static inline l298n_config_t l298n_default_config(void) {
    l298n_config_t cfg = {
        .gpio_enable = -1,
        .gpio_in1 = -1,
        .gpio_in2 = -1,
        .pwm_frequency_hz = L298N_DEFAULT_PWM_FREQ_HZ,
        .active_low = false,
        .enable_reverse = false,
        .min_duty = 0,
        .max_duty = 100,
        .name = "Motor"
    };
    return cfg;
}

/* ============================================================================
 * Motor Control API
 * ========================================================================== */

/**
 * Initialize L298N motor driver
 *
 * Configures GPIO pins and PWM for motor control.
 * Must be called before any other motor functions.
 *
 * @param motor  Motor instance to initialize
 * @param config Configuration with GPIO pins and parameters
 * @return RESULT_OK on success
 *         RESULT_INVALID_PARAM if motor/config is NULL or pins invalid
 *         RESULT_IO_ERROR if GPIO configuration fails
 */
result_t l298n_init(l298n_motor_t *motor, const l298n_config_t *config);

/**
 * Shutdown motor and release resources
 *
 * Stops motor and releases GPIO pins.
 * Safe to call multiple times.
 *
 * @param motor Motor instance
 */
void l298n_destroy(l298n_motor_t *motor);

/**
 * Set motor speed (forward direction)
 *
 * Sets motor to forward direction with specified speed.
 * Speed is clamped to configured min/max duty cycle.
 *
 * @param motor Motor instance
 * @param speed_percent Speed 0-100 (0 = stop, 100 = full speed)
 * @return RESULT_OK on success
 *         RESULT_NOT_INITIALIZED if motor not initialized
 */
result_t l298n_set_speed(l298n_motor_t *motor, uint8_t speed_percent);

/**
 * Set motor speed with direction
 *
 * Sets motor direction and speed.
 * Reverse only works if enable_reverse=true in config.
 *
 * @param motor     Motor instance
 * @param direction Desired direction
 * @param speed_percent Speed 0-100 (ignored for STOP/BRAKE)
 * @return RESULT_OK on success
 *         RESULT_NOT_SUPPORTED if reverse requested but not enabled
 */
result_t l298n_set_direction(l298n_motor_t *motor, l298n_direction_t direction,
                             uint8_t speed_percent);

/**
 * Stop motor (coast)
 *
 * Allows motor to spin down freely (no braking).
 * Equivalent to l298n_set_direction(motor, L298N_DIR_STOP, 0).
 *
 * @param motor Motor instance
 * @return RESULT_OK on success
 */
result_t l298n_stop(l298n_motor_t *motor);

/**
 * Brake motor (active stop)
 *
 * Actively brakes the motor for faster stop.
 * Equivalent to l298n_set_direction(motor, L298N_DIR_BRAKE, 0).
 *
 * @param motor Motor instance
 * @return RESULT_OK on success
 */
result_t l298n_brake(l298n_motor_t *motor);

/**
 * Get current motor status
 *
 * @param motor  Motor instance
 * @param status Output status structure
 * @return RESULT_OK on success
 */
result_t l298n_get_status(const l298n_motor_t *motor, l298n_status_t *status);

/**
 * Check if motor is currently running
 *
 * @param motor Motor instance
 * @return true if motor is running
 */
bool l298n_is_running(const l298n_motor_t *motor);

/**
 * Get current duty cycle
 *
 * @param motor Motor instance
 * @return Current duty cycle (0-100)
 */
uint8_t l298n_get_duty(const l298n_motor_t *motor);

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

/**
 * Convert direction enum to string
 */
const char* l298n_direction_to_string(l298n_direction_t dir);

/**
 * Validate configuration
 *
 * @param config Configuration to validate
 * @return RESULT_OK if valid
 */
result_t l298n_validate_config(const l298n_config_t *config);

#endif /* L298N_DRIVER_H */
