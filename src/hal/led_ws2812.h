/**
 * @file led_ws2812.h
 * @brief WS2812/WS2811 LED Hardware Abstraction Layer
 *
 * Provides a unified interface for driving WS2812/WS2811 addressable RGB LEDs
 * with multiple backend implementations:
 * - SPI: Cross-platform, works on any SBC with SPI
 * - rpi_ws281x: Optimized for Raspberry Pi (PWM+DMA)
 *
 * The appropriate backend is selected at compile time based on:
 * - HAVE_RPI_WS281X: Use rpi_ws281x library (Pi default)
 * - Otherwise: Use SPI backend
 *
 * Feature is completely optional - compile with LED_SUPPORT=OFF for zero overhead.
 */

#ifndef LED_WS2812_H
#define LED_WS2812_H

#include "utils/common.h"

#ifdef LED_SUPPORT

#include <stdint.h>
#include <stdbool.h>

/* Maximum number of LEDs supported */
#define LED_MAX_COUNT       64

/* LED color representation (GRB order for WS2812) */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

/* Backend type enumeration */
typedef enum {
    LED_BACKEND_NONE = 0,
    LED_BACKEND_SPI,
    LED_BACKEND_RPI_WS281X,
    LED_BACKEND_AUTO          /* Auto-detect based on platform */
} led_backend_type_t;

/* LED strip configuration */
typedef struct {
    led_backend_type_t backend;
    uint16_t led_count;
    uint8_t brightness;       /* Global brightness 0-255 */

    /* SPI backend settings */
    char spi_device[32];      /* e.g., "/dev/spidev0.0" */
    uint32_t spi_speed_hz;    /* SPI clock speed (default 2.4MHz for WS2812) */

    /* rpi_ws281x backend settings */
    int gpio_pin;             /* GPIO pin number (BCM) */
    int dma_channel;          /* DMA channel (default 10) */
    int strip_type;           /* WS2811_STRIP_GRB, etc. */
} led_config_t;

/* LED strip state */
typedef struct {
    led_backend_type_t backend;
    uint16_t led_count;
    uint8_t brightness;
    led_color_t *pixels;      /* Pixel buffer */
    void *backend_data;       /* Backend-specific state */
    bool initialized;
} led_strip_t;

/* Backend operations (implemented by each backend) */
typedef struct {
    result_t (*init)(led_strip_t *strip, const led_config_t *config);
    void (*cleanup)(led_strip_t *strip);
    result_t (*render)(led_strip_t *strip);
} led_backend_ops_t;

/**
 * @brief Get default LED configuration
 * @param config Configuration structure to populate
 *
 * Sets sensible defaults including auto-detecting the best backend
 * for the current platform.
 */
void led_config_defaults(led_config_t *config);

/**
 * @brief Initialize LED strip
 * @param strip LED strip state
 * @param config Configuration
 * @return RESULT_OK on success
 */
result_t led_strip_init(led_strip_t *strip, const led_config_t *config);

/**
 * @brief Cleanup LED strip
 * @param strip LED strip to cleanup
 */
void led_strip_cleanup(led_strip_t *strip);

/**
 * @brief Set a single pixel color
 * @param strip LED strip
 * @param index Pixel index (0-based)
 * @param color Color to set
 * @return RESULT_OK on success
 */
result_t led_set_pixel(led_strip_t *strip, uint16_t index, led_color_t color);

/**
 * @brief Set all pixels to the same color
 * @param strip LED strip
 * @param color Color to set
 * @return RESULT_OK on success
 */
result_t led_set_all(led_strip_t *strip, led_color_t color);

/**
 * @brief Clear all pixels (turn off)
 * @param strip LED strip
 * @return RESULT_OK on success
 */
result_t led_clear(led_strip_t *strip);

/**
 * @brief Render pixel buffer to LEDs
 * @param strip LED strip
 * @return RESULT_OK on success
 *
 * Must be called after setting pixels to actually update the LEDs.
 */
result_t led_render(led_strip_t *strip);

/**
 * @brief Set global brightness
 * @param strip LED strip
 * @param brightness Brightness level 0-255
 */
void led_set_brightness(led_strip_t *strip, uint8_t brightness);

/**
 * @brief Create color from RGB values
 * @param r Red component (0-255)
 * @param g Green component (0-255)
 * @param b Blue component (0-255)
 * @return led_color_t structure
 */
static inline led_color_t led_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (led_color_t){ .r = r, .g = g, .b = b };
}

/**
 * @brief Create color from HSV values
 * @param h Hue (0-359)
 * @param s Saturation (0-100)
 * @param v Value/brightness (0-100)
 * @return led_color_t structure
 */
led_color_t led_hsv(uint16_t h, uint8_t s, uint8_t v);

/**
 * @brief Get backend name string
 * @param backend Backend type
 * @return Human-readable backend name
 */
const char *led_backend_name(led_backend_type_t backend);

/**
 * @brief Check if LED support is available on this platform
 * @return true if at least one backend is available
 */
bool led_is_available(void);

/* Backend registration (internal use) */
void led_register_spi_backend(void);
void led_register_rpi_backend(void);

#else /* !LED_SUPPORT */

/* Stub definitions when LED support is disabled - zero overhead */
typedef struct { uint8_t r, g, b; } led_color_t;
typedef struct { int dummy; } led_strip_t;
typedef struct { int dummy; } led_config_t;

static inline void led_config_defaults(led_config_t *c) { UNUSED(c); }
static inline result_t led_strip_init(led_strip_t *s, const led_config_t *c) { UNUSED(s); UNUSED(c); return RESULT_NOT_SUPPORTED; }
static inline void led_strip_cleanup(led_strip_t *s) { UNUSED(s); }
static inline result_t led_set_pixel(led_strip_t *s, uint16_t i, led_color_t c) { UNUSED(s); UNUSED(i); UNUSED(c); return RESULT_NOT_SUPPORTED; }
static inline result_t led_set_all(led_strip_t *s, led_color_t c) { UNUSED(s); UNUSED(c); return RESULT_NOT_SUPPORTED; }
static inline result_t led_clear(led_strip_t *s) { UNUSED(s); return RESULT_NOT_SUPPORTED; }
static inline result_t led_render(led_strip_t *s) { UNUSED(s); return RESULT_NOT_SUPPORTED; }
static inline void led_set_brightness(led_strip_t *s, uint8_t b) { UNUSED(s); UNUSED(b); }
static inline led_color_t led_rgb(uint8_t r, uint8_t g, uint8_t b) { return (led_color_t){ r, g, b }; }
static inline bool led_is_available(void) { return false; }

#endif /* LED_SUPPORT */

#endif /* LED_WS2812_H */
