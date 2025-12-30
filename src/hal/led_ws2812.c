/**
 * @file led_ws2812.c
 * @brief WS2812/WS2811 LED HAL implementation
 */

#include "led_ws2812.h"

#ifdef LED_SUPPORT

#include "utils/logger.h"
#include "platform/board_detect.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Backend operations table */
static const led_backend_ops_t *g_backends[5] = { NULL, NULL, NULL, NULL, NULL };

/* External backend implementations */
extern const led_backend_ops_t led_spi_ops;
extern const led_backend_ops_t led_rp2040_ops;
#ifdef HAVE_RPI_WS281X
extern const led_backend_ops_t led_rpi_ops;
#endif

/* ============================================================================
 * Backend Registration
 * ========================================================================== */

void led_register_spi_backend(void) {
    g_backends[LED_BACKEND_SPI] = &led_spi_ops;
}

void led_register_rp2040_backend(void) {
    g_backends[LED_BACKEND_RP2040] = &led_rp2040_ops;
}

#ifdef HAVE_RPI_WS281X
void led_register_rpi_backend(void) {
    g_backends[LED_BACKEND_RPI_WS281X] = &led_rpi_ops;
}
#else
void led_register_rpi_backend(void) {
    /* No-op when rpi_ws281x not available */
}
#endif

/* ============================================================================
 * Platform Detection
 * ========================================================================== */

/* Check if RP2040 LED controller is connected */
static bool detect_rp2040_device(void) {
    struct stat st;
    /* Check common device paths */
    if (stat("/dev/serial/by-id", &st) == 0) {
        /* Could glob for WaterTreat or Pico devices, but simple check suffices */
        return true;  /* Directory exists, might have device */
    }
    if (stat("/dev/ttyACM0", &st) == 0) {
        return true;
    }
    return false;
}

static led_backend_type_t detect_best_backend(void) {
    /* Prefer RP2040 USB backend - works on any platform */
    if (detect_rp2040_device()) {
        LOG_DEBUG("RP2040 USB device may be available - trying RP2040 backend");
        return LED_BACKEND_RP2040;
    }

    board_info_t board_info;
    if (board_detect(&board_info) != RESULT_OK) {
        LOG_DEBUG("Board detection failed - using SPI backend");
        return LED_BACKEND_SPI;
    }

    /* Use rpi_ws281x on Raspberry Pi if available */
#ifdef HAVE_RPI_WS281X
    switch (board_info.type) {
        case BOARD_RASPBERRY_PI_3:
        case BOARD_RASPBERRY_PI_4:
        case BOARD_RASPBERRY_PI_5:
        case BOARD_RASPBERRY_PI_ZERO:
        case BOARD_RASPBERRY_PI_ZERO2:
            LOG_DEBUG("Detected Raspberry Pi - using rpi_ws281x backend");
            return LED_BACKEND_RPI_WS281X;
        default:
            break;
    }
#endif

    /* Default to SPI for all other boards */
    LOG_DEBUG("Using SPI backend for LED strip");
    return LED_BACKEND_SPI;
}

/* ============================================================================
 * Configuration
 * ========================================================================== */

void led_config_defaults(led_config_t *config) {
    if (!config) return;

    memset(config, 0, sizeof(*config));

    config->backend = LED_BACKEND_AUTO;
    config->led_count = 8;           /* Conservative default */
    config->brightness = 64;         /* 25% brightness - safe for testing */

    /* SPI defaults */
    SAFE_STRNCPY(config->spi_device, "/dev/spidev0.0", sizeof(config->spi_device));
    config->spi_speed_hz = 2400000;  /* 2.4 MHz for WS2812 timing */

    /* rpi_ws281x defaults */
    config->gpio_pin = 18;           /* GPIO 18 (PWM0) */
    config->dma_channel = 10;        /* DMA channel 10 */
    config->strip_type = 0;          /* WS2812_STRIP_GRB */

    /* RP2040 USB defaults */
    config->rp2040_device[0] = '\0'; /* Empty = auto-detect */
    config->rp2040_baud = 115200;
}

/* ============================================================================
 * Strip Management
 * ========================================================================== */

result_t led_strip_init(led_strip_t *strip, const led_config_t *config) {
    CHECK_NULL(strip);
    CHECK_NULL(config);

    memset(strip, 0, sizeof(*strip));

    /* Validate LED count */
    if (config->led_count == 0 || config->led_count > LED_MAX_COUNT) {
        LOG_ERROR("Invalid LED count: %d (max %d)", config->led_count, LED_MAX_COUNT);
        return RESULT_INVALID_PARAM;
    }

    /* Determine backend */
    led_backend_type_t backend = config->backend;
    if (backend == LED_BACKEND_AUTO) {
        backend = detect_best_backend();
    }

    /* Register backends on first use */
    static bool backends_registered = false;
    if (!backends_registered) {
        led_register_spi_backend();
        led_register_rpi_backend();
        led_register_rp2040_backend();
        backends_registered = true;
    }

    /* Validate backend availability */
    if (backend == LED_BACKEND_NONE || !g_backends[backend]) {
        LOG_ERROR("LED backend %d not available", backend);
        return RESULT_NOT_SUPPORTED;
    }

    /* Allocate pixel buffer */
    strip->pixels = calloc(config->led_count, sizeof(led_color_t));
    if (!strip->pixels) {
        LOG_ERROR("Failed to allocate LED pixel buffer");
        return RESULT_NO_MEMORY;
    }

    strip->backend = backend;
    strip->led_count = config->led_count;
    strip->brightness = config->brightness;

    /* Initialize backend */
    result_t result = g_backends[backend]->init(strip, config);
    if (result != RESULT_OK) {
        free(strip->pixels);
        strip->pixels = NULL;
        return result;
    }

    strip->initialized = true;
    LOG_INFO("LED strip initialized: %d LEDs, backend=%s, brightness=%d",
             strip->led_count, led_backend_name(backend), strip->brightness);

    return RESULT_OK;
}

void led_strip_cleanup(led_strip_t *strip) {
    if (!strip || !strip->initialized) return;

    /* Turn off all LEDs */
    led_clear(strip);
    led_render(strip);

    /* Cleanup backend */
    if (g_backends[strip->backend] && g_backends[strip->backend]->cleanup) {
        g_backends[strip->backend]->cleanup(strip);
    }

    /* Free pixel buffer */
    free(strip->pixels);
    strip->pixels = NULL;
    strip->initialized = false;

    LOG_DEBUG("LED strip cleaned up");
}

/* ============================================================================
 * Pixel Operations
 * ========================================================================== */

result_t led_set_pixel(led_strip_t *strip, uint16_t index, led_color_t color) {
    if (!strip || !strip->initialized) return RESULT_NOT_INITIALIZED;
    if (index >= strip->led_count) return RESULT_INVALID_PARAM;

    strip->pixels[index] = color;
    return RESULT_OK;
}

result_t led_set_all(led_strip_t *strip, led_color_t color) {
    if (!strip || !strip->initialized) return RESULT_NOT_INITIALIZED;

    for (uint16_t i = 0; i < strip->led_count; i++) {
        strip->pixels[i] = color;
    }
    return RESULT_OK;
}

result_t led_clear(led_strip_t *strip) {
    return led_set_all(strip, led_rgb(0, 0, 0));
}

result_t led_render(led_strip_t *strip) {
    if (!strip || !strip->initialized) return RESULT_NOT_INITIALIZED;

    if (!g_backends[strip->backend] || !g_backends[strip->backend]->render) {
        return RESULT_NOT_SUPPORTED;
    }

    return g_backends[strip->backend]->render(strip);
}

void led_set_brightness(led_strip_t *strip, uint8_t brightness) {
    if (!strip) return;
    strip->brightness = brightness;
}

/* ============================================================================
 * Color Utilities
 * ========================================================================== */

led_color_t led_hsv(uint16_t h, uint8_t s, uint8_t v) {
    /* HSV to RGB conversion */
    if (s == 0) {
        uint8_t val = (v * 255) / 100;
        return led_rgb(val, val, val);
    }

    h = h % 360;
    uint8_t region = h / 60;
    uint8_t remainder = (h - (region * 60)) * 6;

    uint8_t p = (v * (100 - s)) * 255 / 10000;
    uint8_t q = (v * (100 - (s * remainder) / 360)) * 255 / 10000;
    uint8_t t = (v * (100 - (s * (360 - remainder)) / 360)) * 255 / 10000;
    uint8_t val = (v * 255) / 100;

    switch (region) {
        case 0:  return led_rgb(val, t, p);
        case 1:  return led_rgb(q, val, p);
        case 2:  return led_rgb(p, val, t);
        case 3:  return led_rgb(p, q, val);
        case 4:  return led_rgb(t, p, val);
        default: return led_rgb(val, p, q);
    }
}

const char *led_backend_name(led_backend_type_t backend) {
    switch (backend) {
        case LED_BACKEND_NONE:       return "none";
        case LED_BACKEND_SPI:        return "spi";
        case LED_BACKEND_RPI_WS281X: return "rpi_ws281x";
        case LED_BACKEND_RP2040:     return "rp2040";
        case LED_BACKEND_AUTO:       return "auto";
        default:                     return "unknown";
    }
}

bool led_is_available(void) {
#ifdef HAVE_RPI_WS281X
    return true;
#else
    /* Check if SPI device exists */
    struct stat st;
    return (stat("/dev/spidev0.0", &st) == 0);
#endif
}

#endif /* LED_SUPPORT */
