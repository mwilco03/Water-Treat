/**
 * @file led_backend_rpi.c
 * @brief rpi_ws281x backend for WS2812/WS2811 LEDs on Raspberry Pi
 *
 * Uses the rpi_ws281x library which provides hardware PWM + DMA based
 * LED driving. This is the most reliable method on Raspberry Pi and
 * provides precise timing without CPU overhead.
 *
 * Requires:
 * - Raspberry Pi hardware
 * - rpi_ws281x library installed (libws2811-dev)
 * - Root privileges (for DMA access)
 *
 * Build with -DHAVE_RPI_WS281X=ON and link with -lws2811
 */

#include "led_ws2812.h"

#if defined(LED_SUPPORT) && defined(HAVE_RPI_WS281X)

#include "utils/logger.h"
#include <stdlib.h>
#include <string.h>

/* rpi_ws281x library header */
#include <ws2811/ws2811.h>

/* Backend state */
typedef struct {
    ws2811_t ws2811;
    bool initialized;
} rpi_backend_t;

/* Default strip type if not specified */
#ifndef WS2811_STRIP_GRB
#define WS2811_STRIP_GRB 0x00081000
#endif

/* ============================================================================
 * Backend Operations
 * ========================================================================== */

static result_t rpi_init(led_strip_t *strip, const led_config_t *config) {
    rpi_backend_t *backend = calloc(1, sizeof(rpi_backend_t));
    if (!backend) {
        return RESULT_NO_MEMORY;
    }

    /* Configure ws2811 structure */
    backend->ws2811.freq = 800000;  /* 800 kHz for WS2812 */
    backend->ws2811.dmanum = config->dma_channel;

    /* Channel 0 configuration */
    backend->ws2811.channel[0].gpionum = config->gpio_pin;
    backend->ws2811.channel[0].invert = 0;
    backend->ws2811.channel[0].count = strip->led_count;
    backend->ws2811.channel[0].strip_type = config->strip_type ? config->strip_type : WS2811_STRIP_GRB;
    backend->ws2811.channel[0].brightness = strip->brightness;

    /* Channel 1 unused */
    backend->ws2811.channel[1].gpionum = 0;
    backend->ws2811.channel[1].invert = 0;
    backend->ws2811.channel[1].count = 0;
    backend->ws2811.channel[1].brightness = 0;

    /* Initialize the library */
    ws2811_return_t ret = ws2811_init(&backend->ws2811);
    if (ret != WS2811_SUCCESS) {
        LOG_ERROR("rpi_ws281x init failed: %s", ws2811_get_return_t_str(ret));
        free(backend);
        return RESULT_ERROR;
    }

    backend->initialized = true;
    strip->backend_data = backend;

    LOG_DEBUG("rpi_ws281x backend initialized: GPIO=%d, DMA=%d, count=%d",
              config->gpio_pin, config->dma_channel, strip->led_count);

    return RESULT_OK;
}

static void rpi_cleanup(led_strip_t *strip) {
    rpi_backend_t *backend = (rpi_backend_t *)strip->backend_data;
    if (!backend) return;

    if (backend->initialized) {
        /* Clear LEDs before shutdown */
        for (int i = 0; i < backend->ws2811.channel[0].count; i++) {
            backend->ws2811.channel[0].leds[i] = 0;
        }
        ws2811_render(&backend->ws2811);
        ws2811_fini(&backend->ws2811);
    }

    free(backend);
    strip->backend_data = NULL;
}

static result_t rpi_render(led_strip_t *strip) {
    rpi_backend_t *backend = (rpi_backend_t *)strip->backend_data;
    if (!backend || !backend->initialized) {
        return RESULT_INVALID_STATE;
    }

    /* Update brightness if changed */
    backend->ws2811.channel[0].brightness = strip->brightness;

    /* Copy pixel data to ws2811 buffer */
    for (uint16_t i = 0; i < strip->led_count; i++) {
        led_color_t color = strip->pixels[i];
        /* ws2811 uses 0xWWRRGGBB format (W=white for RGBW strips, 0 for RGB) */
        backend->ws2811.channel[0].leds[i] =
            ((uint32_t)color.r << 16) |
            ((uint32_t)color.g << 8) |
            ((uint32_t)color.b);
    }

    /* Render to LEDs */
    ws2811_return_t ret = ws2811_render(&backend->ws2811);
    if (ret != WS2811_SUCCESS) {
        LOG_ERROR("rpi_ws281x render failed: %s", ws2811_get_return_t_str(ret));
        return RESULT_IO_ERROR;
    }

    return RESULT_OK;
}

/* Backend operations structure */
const led_backend_ops_t led_rpi_ops = {
    .init = rpi_init,
    .cleanup = rpi_cleanup,
    .render = rpi_render
};

#endif /* LED_SUPPORT && HAVE_RPI_WS281X */
