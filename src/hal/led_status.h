/**
 * @file led_status.h
 * @brief LED Status Indicator Manager
 *
 * High-level LED status management following ISA-101 / IEC 60073 color standards
 * for industrial process control status indication.
 *
 * Color Standards:
 * - GREEN:   Normal operation, running, OK
 * - YELLOW:  Warning, attention required, caution
 * - RED:     Alarm, fault, emergency, stop
 * - BLUE:    Manual mode, operator intervention
 * - CYAN:    Communication active, data transfer
 * - MAGENTA: Calibration mode, maintenance
 * - WHITE:   Power on, neutral, standby
 * - OFF:     Disabled, not applicable
 *
 * LED Assignment (default 8 LEDs):
 * - LED 0: System status
 * - LED 1: PROFINET connection
 * - LED 2-5: Sensor status (configurable)
 * - LED 6-7: Actuator status (configurable)
 */

#ifndef LED_STATUS_H
#define LED_STATUS_H

#include "led_ws2812.h"

#ifdef LED_SUPPORT

/* ============================================================================
 * ISA-101 / IEC 60073 Color Definitions
 * ========================================================================== */

/* Standard industrial colors */
#define LED_COLOR_OFF       led_rgb(0, 0, 0)
#define LED_COLOR_GREEN     led_rgb(0, 255, 0)      /* Normal/OK */
#define LED_COLOR_YELLOW    led_rgb(255, 200, 0)    /* Warning */
#define LED_COLOR_RED       led_rgb(255, 0, 0)      /* Alarm/Fault */
#define LED_COLOR_BLUE      led_rgb(0, 0, 255)      /* Manual mode */
#define LED_COLOR_CYAN      led_rgb(0, 255, 255)    /* Communication */
#define LED_COLOR_MAGENTA   led_rgb(255, 0, 255)    /* Calibration */
#define LED_COLOR_WHITE     led_rgb(255, 255, 255)  /* Power/Standby */

/* Dimmed variants (for pulsing/breathing effects) */
#define LED_COLOR_DIM_GREEN     led_rgb(0, 64, 0)
#define LED_COLOR_DIM_YELLOW    led_rgb(64, 50, 0)
#define LED_COLOR_DIM_RED       led_rgb(64, 0, 0)

/* ============================================================================
 * Status Definitions
 * ========================================================================== */

/* System status levels */
typedef enum {
    LED_STATUS_OFF = 0,       /* LED disabled */
    LED_STATUS_OK,            /* Normal operation (solid green) */
    LED_STATUS_WARNING,       /* Warning (blinking yellow) */
    LED_STATUS_ALARM,         /* Alarm (fast blinking red) */
    LED_STATUS_FAULT,         /* Fault (solid red) */
    LED_STATUS_MANUAL,        /* Manual mode (solid blue) */
    LED_STATUS_COMM_ACTIVE,   /* Communication (blinking cyan) */
    LED_STATUS_CALIBRATING,   /* Calibration (pulsing magenta) */
    LED_STATUS_STANDBY,       /* Standby (dim white) */
    LED_STATUS_INITIALIZING   /* Starting up (pulsing white) */
} led_status_level_t;

/* LED function assignment */
typedef enum {
    LED_FUNC_SYSTEM = 0,      /* Overall system status */
    LED_FUNC_PROFINET,        /* PROFINET connection status */
    LED_FUNC_SENSOR_1,        /* First sensor status */
    LED_FUNC_SENSOR_2,
    LED_FUNC_SENSOR_3,
    LED_FUNC_SENSOR_4,
    LED_FUNC_ACTUATOR_1,      /* First actuator status */
    LED_FUNC_ACTUATOR_2,
    LED_FUNC_MAX
} led_function_t;

/* Animation patterns */
typedef enum {
    LED_ANIM_SOLID = 0,       /* Steady on */
    LED_ANIM_BLINK_SLOW,      /* 1 Hz blink */
    LED_ANIM_BLINK_FAST,      /* 4 Hz blink */
    LED_ANIM_PULSE,           /* Smooth breathing */
    LED_ANIM_FLASH            /* Brief flash then off */
} led_animation_t;

/* Per-LED state */
typedef struct {
    led_status_level_t status;
    led_color_t color;
    led_animation_t animation;
    uint32_t animation_phase;  /* Animation state counter */
} led_state_t;

/* Status manager state */
typedef struct {
    led_strip_t strip;
    led_state_t leds[LED_MAX_COUNT];
    uint32_t update_counter;   /* For animation timing */
    uint16_t led_count;
    bool enabled;
    bool initialized;
} led_status_manager_t;

/* ============================================================================
 * Manager Functions
 * ========================================================================== */

/**
 * @brief Initialize LED status manager
 * @param mgr Manager instance
 * @param config LED configuration
 * @return RESULT_OK on success
 */
result_t led_status_init(led_status_manager_t *mgr, const led_config_t *config);

/**
 * @brief Cleanup LED status manager
 * @param mgr Manager instance
 */
void led_status_cleanup(led_status_manager_t *mgr);

/**
 * @brief Update LED animations (call periodically, ~50-100Hz)
 * @param mgr Manager instance
 *
 * This function updates animation states and renders the LEDs.
 * Should be called from a timer or main loop.
 */
void led_status_update(led_status_manager_t *mgr);

/**
 * @brief Set LED status by function
 * @param mgr Manager instance
 * @param func LED function (e.g., LED_FUNC_SYSTEM)
 * @param status Status level
 */
void led_set_status(led_status_manager_t *mgr, led_function_t func,
                    led_status_level_t status);

/**
 * @brief Set custom LED color and animation
 * @param mgr Manager instance
 * @param index LED index
 * @param color Custom color
 * @param animation Animation pattern
 */
void led_set_custom(led_status_manager_t *mgr, uint16_t index,
                    led_color_t color, led_animation_t animation);

/**
 * @brief Enable or disable LED output
 * @param mgr Manager instance
 * @param enable true to enable, false to disable
 */
void led_status_enable(led_status_manager_t *mgr, bool enable);

/**
 * @brief Set all LEDs to a test pattern
 * @param mgr Manager instance
 *
 * Useful for verifying LED wiring and configuration.
 */
void led_status_test(led_status_manager_t *mgr);

/* ============================================================================
 * Convenience Functions for System Integration
 * ========================================================================== */

/**
 * @brief Set system status LED
 * @param mgr Manager instance
 * @param status System status
 */
static inline void led_set_system_status(led_status_manager_t *mgr,
                                          led_status_level_t status) {
    led_set_status(mgr, LED_FUNC_SYSTEM, status);
}

/**
 * @brief Set PROFINET connection status LED
 * @param mgr Manager instance
 * @param connected true if connected to controller
 * @param active true if data exchange active
 */
void led_set_profinet_status(led_status_manager_t *mgr, bool connected, bool active);

/**
 * @brief Set sensor status LED
 * @param mgr Manager instance
 * @param sensor_index Sensor index (0-3)
 * @param has_alarm true if sensor in alarm
 * @param has_warning true if sensor in warning
 * @param is_calibrating true if sensor being calibrated
 */
void led_set_sensor_status(led_status_manager_t *mgr, int sensor_index,
                           bool has_alarm, bool has_warning, bool is_calibrating);

/**
 * @brief Set actuator status LED
 * @param mgr Manager instance
 * @param actuator_index Actuator index (0-1)
 * @param is_active true if actuator is on/running
 * @param is_manual true if in manual mode
 * @param has_fault true if actuator fault detected
 */
void led_set_actuator_status(led_status_manager_t *mgr, int actuator_index,
                             bool is_active, bool is_manual, bool has_fault);

/**
 * @brief Get status color for a status level
 * @param status Status level
 * @return Corresponding ISA-101 color
 */
led_color_t led_status_to_color(led_status_level_t status);

/**
 * @brief Get status name string
 * @param status Status level
 * @return Human-readable status name
 */
const char *led_status_name(led_status_level_t status);

#else /* !LED_SUPPORT */

/* Stub definitions for when LED support is disabled */
typedef struct { int dummy; } led_status_manager_t;
typedef int led_function_t;
typedef int led_status_level_t;
typedef int led_animation_t;

#define LED_COLOR_OFF       led_rgb(0,0,0)
#define LED_COLOR_GREEN     led_rgb(0,0,0)
#define LED_COLOR_YELLOW    led_rgb(0,0,0)
#define LED_COLOR_RED       led_rgb(0,0,0)

static inline result_t led_status_init(led_status_manager_t *m, const led_config_t *c) { UNUSED(m); UNUSED(c); return RESULT_NOT_SUPPORTED; }
static inline void led_status_cleanup(led_status_manager_t *m) { UNUSED(m); }
static inline void led_status_update(led_status_manager_t *m) { UNUSED(m); }
static inline void led_set_status(led_status_manager_t *m, led_function_t f, led_status_level_t s) { UNUSED(m); UNUSED(f); UNUSED(s); }
static inline void led_set_system_status(led_status_manager_t *m, led_status_level_t s) { UNUSED(m); UNUSED(s); }
static inline void led_set_profinet_status(led_status_manager_t *m, bool c, bool a) { UNUSED(m); UNUSED(c); UNUSED(a); }
static inline void led_set_sensor_status(led_status_manager_t *m, int i, bool a, bool w, bool c) { UNUSED(m); UNUSED(i); UNUSED(a); UNUSED(w); UNUSED(c); }
static inline void led_set_actuator_status(led_status_manager_t *m, int i, bool a, bool man, bool f) { UNUSED(m); UNUSED(i); UNUSED(a); UNUSED(man); UNUSED(f); }

#endif /* LED_SUPPORT */

#endif /* LED_STATUS_H */
