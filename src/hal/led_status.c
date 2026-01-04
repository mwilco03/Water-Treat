/**
 * @file led_status.c
 * @brief LED Status Indicator Manager Implementation
 */

#include "led_status.h"

#ifdef LED_SUPPORT

#include "utils/logger.h"
#include <string.h>

/* ============================================================================
 * Animation Timing Constants
 * ============================================================================
 * All timing is derived from the LED update rate and desired frequencies.
 * This makes the relationship between values explicit and self-documenting.
 */
#define LED_UPDATE_RATE_HZ      50      /* LED update frequency */

/* Blink frequencies */
#define BLINK_SLOW_HZ           1       /* Slow blink: 1 Hz */
#define BLINK_FAST_HZ           4       /* Fast blink: 4 Hz */
#define PULSE_CYCLE_SEC         2       /* Pulse cycle: 2 seconds */
#define FLASH_DURATION_MS       100     /* Flash duration: 100ms */

/* Calculated animation periods (in ticks) */
#define ANIM_BLINK_SLOW_PERIOD  (LED_UPDATE_RATE_HZ / BLINK_SLOW_HZ)
#define ANIM_BLINK_FAST_PERIOD  (LED_UPDATE_RATE_HZ / BLINK_FAST_HZ)
#define ANIM_PULSE_PERIOD       (LED_UPDATE_RATE_HZ * PULSE_CYCLE_SEC)
#define ANIM_FLASH_DURATION     (LED_UPDATE_RATE_HZ * FLASH_DURATION_MS / 1000)

/* ============================================================================
 * Status Mapping Lookup Table
 * ============================================================================
 * Consolidated table replaces three separate switch statements.
 * Order MUST match led_status_level_t enum values.
 */
typedef struct {
    led_color_t     color;
    led_animation_t animation;
    const char     *name;
} led_status_info_t;

static const led_status_info_t status_table[] = {
    /* LED_STATUS_OFF */          { LED_COLOR_OFF,     LED_ANIM_SOLID,      "off"          },
    /* LED_STATUS_OK */           { LED_COLOR_GREEN,   LED_ANIM_SOLID,      "ok"           },
    /* LED_STATUS_WARNING */      { LED_COLOR_YELLOW,  LED_ANIM_BLINK_SLOW, "warning"      },
    /* LED_STATUS_ALARM */        { LED_COLOR_RED,     LED_ANIM_BLINK_FAST, "alarm"        },
    /* LED_STATUS_FAULT */        { LED_COLOR_RED,     LED_ANIM_SOLID,      "fault"        },
    /* LED_STATUS_MANUAL */       { LED_COLOR_BLUE,    LED_ANIM_SOLID,      "manual"       },
    /* LED_STATUS_COMM_ACTIVE */  { LED_COLOR_CYAN,    LED_ANIM_BLINK_FAST, "comm_active"  },
    /* LED_STATUS_CALIBRATING */  { LED_COLOR_MAGENTA, LED_ANIM_PULSE,      "calibrating"  },
    /* LED_STATUS_STANDBY */      { LED_COLOR_WHITE,   LED_ANIM_SOLID,      "standby"      },
    /* LED_STATUS_INITIALIZING */ { LED_COLOR_WHITE,   LED_ANIM_PULSE,      "initializing" },
};

#define STATUS_TABLE_SIZE (sizeof(status_table) / sizeof(status_table[0]))

/* Accessor functions use the lookup table */

led_color_t led_status_to_color(led_status_level_t status) {
    if (status >= STATUS_TABLE_SIZE) return LED_COLOR_OFF;
    return status_table[status].color;
}

static led_animation_t status_to_animation(led_status_level_t status) {
    if (status >= STATUS_TABLE_SIZE) return LED_ANIM_SOLID;
    return status_table[status].animation;
}

const char *led_status_name(led_status_level_t status) {
    if (status >= STATUS_TABLE_SIZE) return "unknown";
    return status_table[status].name;
}

/* ============================================================================
 * Animation Processing
 * ========================================================================== */

static led_color_t apply_animation(led_state_t *state, uint32_t counter) {
    led_color_t base = state->color;

    switch (state->animation) {
        case LED_ANIM_SOLID:
            return base;

        case LED_ANIM_BLINK_SLOW: {
            uint32_t phase = counter % ANIM_BLINK_SLOW_PERIOD;
            return (phase < ANIM_BLINK_SLOW_PERIOD / 2) ? base : LED_COLOR_OFF;
        }

        case LED_ANIM_BLINK_FAST: {
            uint32_t phase = counter % ANIM_BLINK_FAST_PERIOD;
            return (phase < ANIM_BLINK_FAST_PERIOD / 2) ? base : LED_COLOR_OFF;
        }

        case LED_ANIM_PULSE: {
            /* Sinusoidal brightness modulation */
            uint32_t phase = counter % ANIM_PULSE_PERIOD;
            /* Simple triangle wave for brightness */
            int brightness;
            if (phase < ANIM_PULSE_PERIOD / 2) {
                brightness = (phase * 255) / (ANIM_PULSE_PERIOD / 2);
            } else {
                brightness = 255 - ((phase - ANIM_PULSE_PERIOD / 2) * 255) / (ANIM_PULSE_PERIOD / 2);
            }
            /* Minimum brightness of 20% to keep LED visible */
            brightness = 50 + (brightness * 205) / 255;
            return led_rgb(
                (base.r * brightness) / 255,
                (base.g * brightness) / 255,
                (base.b * brightness) / 255
            );
        }

        case LED_ANIM_FLASH: {
            if (state->animation_phase > 0) {
                state->animation_phase--;
                return base;
            }
            return LED_COLOR_OFF;
        }

        default:
            return base;
    }
}

/* ============================================================================
 * Manager Functions
 * ========================================================================== */

result_t led_status_init(led_status_manager_t *mgr, const led_config_t *config) {
    CHECK_NULL(mgr);
    CHECK_NULL(config);

    memset(mgr, 0, sizeof(*mgr));

    /* Initialize the underlying LED strip */
    result_t result = led_strip_init(&mgr->strip, config);
    if (result != RESULT_OK) {
        LOG_WARNING("LED strip initialization failed: %d", result);
        return result;
    }

    mgr->led_count = config->led_count;
    mgr->enabled = true;
    mgr->initialized = true;

    /* Set all LEDs to initializing state */
    for (int i = 0; i < mgr->led_count && i < LED_MAX_COUNT; i++) {
        mgr->leds[i].status = LED_STATUS_INITIALIZING;
        mgr->leds[i].color = led_status_to_color(LED_STATUS_INITIALIZING);
        mgr->leds[i].animation = LED_ANIM_PULSE;
    }

    LOG_INFO("LED status manager initialized with %d LEDs", mgr->led_count);
    return RESULT_OK;
}

void led_status_cleanup(led_status_manager_t *mgr) {
    if (!mgr || !mgr->initialized) return;

    led_strip_cleanup(&mgr->strip);
    mgr->initialized = false;
    LOG_DEBUG("LED status manager cleaned up");
}

void led_status_update(led_status_manager_t *mgr) {
    if (!mgr || !mgr->initialized || !mgr->enabled) return;

    mgr->update_counter++;

    /* Apply animations and update pixel buffer */
    for (int i = 0; i < mgr->led_count && i < LED_MAX_COUNT; i++) {
        led_color_t color = apply_animation(&mgr->leds[i], mgr->update_counter);
        led_set_pixel(&mgr->strip, i, color);
    }

    /* Render to hardware */
    led_render(&mgr->strip);
}

void led_set_status(led_status_manager_t *mgr, led_function_t func,
                    led_status_level_t status) {
    if (!mgr || !mgr->initialized) return;
    if (func >= mgr->led_count || func >= LED_FUNC_MAX) return;

    mgr->leds[func].status = status;
    mgr->leds[func].color = led_status_to_color(status);
    mgr->leds[func].animation = status_to_animation(status);
    mgr->leds[func].animation_phase = 0;
}

void led_set_custom(led_status_manager_t *mgr, uint16_t index,
                    led_color_t color, led_animation_t animation) {
    if (!mgr || !mgr->initialized) return;
    if (index >= mgr->led_count) return;

    mgr->leds[index].status = LED_STATUS_OFF;  /* Custom override */
    mgr->leds[index].color = color;
    mgr->leds[index].animation = animation;
    mgr->leds[index].animation_phase = 0;
}

void led_status_enable(led_status_manager_t *mgr, bool enable) {
    if (!mgr || !mgr->initialized) return;

    mgr->enabled = enable;

    if (!enable) {
        /* Turn off all LEDs when disabled */
        led_clear(&mgr->strip);
        led_render(&mgr->strip);
    }
}

void led_status_test(led_status_manager_t *mgr) {
    if (!mgr || !mgr->initialized) return;

    LOG_INFO("Running LED test pattern");

    /* Test each color */
    const led_color_t test_colors[] = {
        LED_COLOR_RED, LED_COLOR_GREEN, LED_COLOR_BLUE,
        LED_COLOR_YELLOW, LED_COLOR_CYAN, LED_COLOR_MAGENTA,
        LED_COLOR_WHITE, LED_COLOR_OFF
    };
    const int num_colors = sizeof(test_colors) / sizeof(test_colors[0]);

    for (int c = 0; c < num_colors; c++) {
        led_set_all(&mgr->strip, test_colors[c]);
        led_render(&mgr->strip);
        /* Caller should add delay between colors */
    }
}

/* ============================================================================
 * System Integration Functions
 * ========================================================================== */

void led_set_profinet_status(led_status_manager_t *mgr, bool connected, bool active) {
    if (!mgr || !mgr->initialized) return;

    led_status_level_t status;
    if (!connected) {
        status = LED_STATUS_FAULT;
    } else if (active) {
        status = LED_STATUS_COMM_ACTIVE;
    } else {
        status = LED_STATUS_OK;
    }

    led_set_status(mgr, LED_FUNC_PROFINET, status);
}

void led_set_sensor_status(led_status_manager_t *mgr, int sensor_index,
                           bool has_alarm, bool has_warning, bool is_calibrating) {
    if (!mgr || !mgr->initialized) return;
    if (sensor_index < 0 || sensor_index > 3) return;

    led_function_t func = LED_FUNC_SENSOR_1 + sensor_index;
    if (func >= mgr->led_count) return;

    led_status_level_t status;
    if (is_calibrating) {
        status = LED_STATUS_CALIBRATING;
    } else if (has_alarm) {
        status = LED_STATUS_ALARM;
    } else if (has_warning) {
        status = LED_STATUS_WARNING;
    } else {
        status = LED_STATUS_OK;
    }

    led_set_status(mgr, func, status);
}

void led_set_actuator_status(led_status_manager_t *mgr, int actuator_index,
                             bool is_active, bool is_manual, bool has_fault) {
    if (!mgr || !mgr->initialized) return;
    if (actuator_index < 0 || actuator_index > 1) return;

    led_function_t func = LED_FUNC_ACTUATOR_1 + actuator_index;
    if (func >= mgr->led_count) return;

    led_status_level_t status;
    if (has_fault) {
        status = LED_STATUS_FAULT;
    } else if (is_manual) {
        status = LED_STATUS_MANUAL;
    } else if (is_active) {
        status = LED_STATUS_OK;
    } else {
        status = LED_STATUS_STANDBY;
    }

    led_set_status(mgr, func, status);
}

#endif /* LED_SUPPORT */
