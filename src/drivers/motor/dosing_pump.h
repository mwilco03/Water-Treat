/**
 * @file dosing_pump.h
 * @brief Dosing Pump Control using L298N Motor Driver
 *
 * High-level interface for controlling dosing pumps via L298N motor driver.
 * Automatically detects board type and uses appropriate GPIO pins.
 *
 * Usage:
 *   dosing_pump_t pump;
 *
 *   // Auto-detect board and initialize with default pins
 *   dosing_pump_init_auto(&pump, "ChlorinePump");
 *
 *   // Or specify custom pins
 *   dosing_pump_config_t cfg = dosing_pump_default_config();
 *   cfg.gpio_enable = 18;
 *   cfg.gpio_in1 = 24;
 *   cfg.gpio_in2 = 25;
 *   dosing_pump_init(&pump, &cfg);
 *
 *   // Control the pump
 *   dosing_pump_set_rate(&pump, 50);  // 50% flow rate
 *   dosing_pump_dose_ml(&pump, 10.0, 5.0);  // Dose 10mL at 5mL/sec
 *   dosing_pump_stop(&pump);
 *
 *   dosing_pump_destroy(&pump);
 */

#ifndef DOSING_PUMP_H
#define DOSING_PUMP_H

#include "common.h"
#include "l298n_driver.h"

/* ============================================================================
 * Configuration
 * ========================================================================== */

/** Default pump flow rate calibration (mL per minute at 100% duty) */
#define DOSING_PUMP_DEFAULT_ML_PER_MIN 100.0f

/** Minimum duty cycle for pump to overcome inertia */
#define DOSING_PUMP_MIN_DUTY 15

/** Default PWM frequency for dosing pumps (Hz) */
#define DOSING_PUMP_PWM_FREQ_HZ 1000

/* ============================================================================
 * Types
 * ========================================================================== */

/**
 * Dosing pump configuration
 */
typedef struct {
    int gpio_enable;            /**< PWM enable pin (speed control) */
    int gpio_in1;               /**< Direction pin 1 */
    int gpio_in2;               /**< Direction pin 2 */
    int pwm_frequency_hz;       /**< PWM frequency */
    uint8_t min_duty;           /**< Minimum duty (stall threshold) */
    uint8_t max_duty;           /**< Maximum duty (safety limit) */
    float ml_per_min_at_100;    /**< Flow rate calibration */
    char name[MAX_NAME_LEN];    /**< Pump name for logging */
} dosing_pump_config_t;

/**
 * Dosing pump instance
 */
typedef struct {
    l298n_motor_t motor;        /**< Underlying motor driver */
    dosing_pump_config_t config; /**< Configuration */
    float total_ml_dispensed;   /**< Total volume dispensed (lifetime) */
    bool is_initialized;        /**< Initialization status */
} dosing_pump_t;

/**
 * Pump status for monitoring
 */
typedef struct {
    bool initialized;
    bool running;
    uint8_t duty_cycle;
    float current_flow_ml_min;
    float total_ml_dispensed;
    uint64_t run_time_ms;
} dosing_pump_status_t;

/* ============================================================================
 * Configuration Helpers
 * ========================================================================== */

/**
 * Get default dosing pump configuration
 *
 * Returns configuration with sensible defaults.
 * GPIO pins are set to -1 (invalid) - use dosing_pump_init_auto()
 * for automatic pin detection, or set pins manually.
 */
static inline dosing_pump_config_t dosing_pump_default_config(void) {
    dosing_pump_config_t cfg = {
        .gpio_enable = -1,
        .gpio_in1 = -1,
        .gpio_in2 = -1,
        .pwm_frequency_hz = DOSING_PUMP_PWM_FREQ_HZ,
        .min_duty = DOSING_PUMP_MIN_DUTY,
        .max_duty = 100,
        .ml_per_min_at_100 = DOSING_PUMP_DEFAULT_ML_PER_MIN,
        .name = "DosingPump"
    };
    return cfg;
}

/* ============================================================================
 * Initialization API
 * ========================================================================== */

/**
 * Initialize dosing pump with automatic board detection
 *
 * Detects the current SBC and uses the pre-configured motor pins.
 * This is the recommended initialization method for board-agnostic code.
 *
 * @param pump  Pump instance to initialize
 * @param name  Pump name for logging (e.g., "ChlorinePump")
 * @return RESULT_OK on success
 *         RESULT_NOT_FOUND if board detection fails
 *         RESULT_NOT_SUPPORTED if board has no motor pins configured
 */
result_t dosing_pump_init_auto(dosing_pump_t *pump, const char *name);

/**
 * Initialize dosing pump with explicit configuration
 *
 * Use this when you need to specify custom GPIO pins or parameters.
 *
 * @param pump   Pump instance to initialize
 * @param config Configuration with GPIO pins and parameters
 * @return RESULT_OK on success
 */
result_t dosing_pump_init(dosing_pump_t *pump, const dosing_pump_config_t *config);

/**
 * Shutdown pump and release resources
 */
void dosing_pump_destroy(dosing_pump_t *pump);

/* ============================================================================
 * Pump Control API
 * ========================================================================== */

/**
 * Set pump flow rate
 *
 * @param pump Pump instance
 * @param percent Flow rate 0-100 (0 = stop, 100 = max flow)
 * @return RESULT_OK on success
 */
result_t dosing_pump_set_rate(dosing_pump_t *pump, uint8_t percent);

/**
 * Dispense a specific volume (blocking)
 *
 * Runs pump at specified rate until volume is dispensed.
 * This function blocks until dosing is complete.
 *
 * @param pump     Pump instance
 * @param ml       Volume to dispense in milliliters
 * @param rate_percent Flow rate (10-100%)
 * @return RESULT_OK on success
 *         RESULT_INVALID_PARAM if ml <= 0 or rate out of range
 */
result_t dosing_pump_dose_ml(dosing_pump_t *pump, float ml, uint8_t rate_percent);

/**
 * Start continuous pumping
 *
 * @param pump Pump instance
 * @param rate_percent Flow rate (1-100%)
 * @return RESULT_OK on success
 */
result_t dosing_pump_start(dosing_pump_t *pump, uint8_t rate_percent);

/**
 * Stop pump
 *
 * @param pump Pump instance
 * @return RESULT_OK on success
 */
result_t dosing_pump_stop(dosing_pump_t *pump);

/**
 * Prime the pump (run briefly to fill tubing)
 *
 * Runs pump at high rate for a few seconds.
 *
 * @param pump       Pump instance
 * @param duration_ms Prime duration in milliseconds (default: 3000)
 * @return RESULT_OK on success
 */
result_t dosing_pump_prime(dosing_pump_t *pump, uint32_t duration_ms);

/* ============================================================================
 * Status & Calibration API
 * ========================================================================== */

/**
 * Get current pump status
 *
 * @param pump   Pump instance
 * @param status Output status structure
 * @return RESULT_OK on success
 */
result_t dosing_pump_get_status(const dosing_pump_t *pump, dosing_pump_status_t *status);

/**
 * Check if pump is currently running
 */
bool dosing_pump_is_running(const dosing_pump_t *pump);

/**
 * Set flow rate calibration
 *
 * @param pump          Pump instance
 * @param ml_per_min    Flow rate at 100% duty (mL/min)
 * @return RESULT_OK on success
 */
result_t dosing_pump_set_calibration(dosing_pump_t *pump, float ml_per_min);

/**
 * Reset total dispensed counter
 */
void dosing_pump_reset_counter(dosing_pump_t *pump);

#endif /* DOSING_PUMP_H */
