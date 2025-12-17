/**
 * @file led_backend_spi.c
 * @brief SPI backend for WS2812/WS2811 LEDs
 *
 * Uses SPI to generate WS2812 timing signals. Each WS2812 bit is encoded
 * as 3 SPI bits at 2.4 MHz:
 * - WS2812 '0' bit: 100 (high-low-low) ~0.4us high, 0.85us low
 * - WS2812 '1' bit: 110 (high-high-low) ~0.8us high, 0.45us low
 *
 * This approach works on any SBC with SPI and doesn't require root access
 * or special kernel modules.
 */

#include "led_ws2812.h"

#ifdef LED_SUPPORT

#include "utils/logger.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#ifdef __linux__
#include <linux/spi/spidev.h>
#endif

/* SPI backend state */
typedef struct {
    int fd;                    /* SPI file descriptor */
    uint8_t *tx_buffer;        /* Transmission buffer */
    size_t tx_buffer_size;     /* Buffer size in bytes */
    uint32_t speed_hz;         /* SPI clock speed */
} spi_backend_t;

/* Bytes per LED: 24 bits * 3 SPI bits per WS2812 bit / 8 = 9 bytes per LED */
#define SPI_BYTES_PER_LED   9

/* Reset frame: 50us at 2.4MHz = 120 bits = 15 bytes of zeros */
#define SPI_RESET_BYTES     15

/* Bit patterns for encoding (3 SPI bits per WS2812 bit) */
#define BIT_0_PATTERN       0x04  /* 100 binary */
#define BIT_1_PATTERN       0x06  /* 110 binary */

/* ============================================================================
 * SPI Encoding
 * ========================================================================== */

/**
 * @brief Encode a single byte into SPI bit pattern
 * @param byte Input byte (color component)
 * @param out Output buffer (3 bytes for 8 bits * 3 SPI bits)
 */
static void encode_byte(uint8_t byte, uint8_t *out) {
    /* Each input bit becomes 3 output bits
     * 8 input bits -> 24 output bits -> 3 output bytes */
    uint32_t encoded = 0;

    for (int i = 7; i >= 0; i--) {
        encoded <<= 3;
        if (byte & (1 << i)) {
            encoded |= BIT_1_PATTERN;  /* 110 for '1' */
        } else {
            encoded |= BIT_0_PATTERN;  /* 100 for '0' */
        }
    }

    /* Output as big-endian */
    out[0] = (encoded >> 16) & 0xFF;
    out[1] = (encoded >> 8) & 0xFF;
    out[2] = encoded & 0xFF;
}

/**
 * @brief Encode LED pixel data into SPI buffer
 * @param strip LED strip state
 * @param backend SPI backend state
 */
static void encode_pixels(led_strip_t *strip, spi_backend_t *backend) {
    uint8_t *ptr = backend->tx_buffer;

    /* Encode each pixel (GRB order for WS2812) */
    for (uint16_t i = 0; i < strip->led_count; i++) {
        led_color_t color = strip->pixels[i];

        /* Apply brightness scaling */
        uint8_t g = (color.g * strip->brightness) >> 8;
        uint8_t r = (color.r * strip->brightness) >> 8;
        uint8_t b = (color.b * strip->brightness) >> 8;

        /* Encode GRB order */
        encode_byte(g, ptr);
        ptr += 3;
        encode_byte(r, ptr);
        ptr += 3;
        encode_byte(b, ptr);
        ptr += 3;
    }

    /* Add reset frame (zeros) */
    memset(ptr, 0, SPI_RESET_BYTES);
}

/* ============================================================================
 * Backend Operations
 * ========================================================================== */

static result_t spi_init(led_strip_t *strip, const led_config_t *config) {
#ifdef __linux__
    spi_backend_t *backend = calloc(1, sizeof(spi_backend_t));
    if (!backend) {
        return RESULT_NO_MEMORY;
    }

    /* Open SPI device */
    backend->fd = open(config->spi_device, O_RDWR);
    if (backend->fd < 0) {
        LOG_ERROR("Failed to open SPI device %s: %s",
                  config->spi_device, strerror(errno));
        free(backend);
        return RESULT_IO_ERROR;
    }

    /* Configure SPI */
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    backend->speed_hz = config->spi_speed_hz;

    if (ioctl(backend->fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(backend->fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(backend->fd, SPI_IOC_WR_MAX_SPEED_HZ, &backend->speed_hz) < 0) {
        LOG_ERROR("Failed to configure SPI: %s", strerror(errno));
        close(backend->fd);
        free(backend);
        return RESULT_IO_ERROR;
    }

    /* Allocate transmission buffer */
    backend->tx_buffer_size = (strip->led_count * SPI_BYTES_PER_LED) + SPI_RESET_BYTES;
    backend->tx_buffer = calloc(1, backend->tx_buffer_size);
    if (!backend->tx_buffer) {
        close(backend->fd);
        free(backend);
        return RESULT_NO_MEMORY;
    }

    strip->backend_data = backend;

    LOG_DEBUG("SPI backend initialized: device=%s, speed=%u Hz, buffer=%zu bytes",
              config->spi_device, backend->speed_hz, backend->tx_buffer_size);

    return RESULT_OK;
#else
    UNUSED(strip);
    UNUSED(config);
    LOG_ERROR("SPI backend requires Linux");
    return RESULT_NOT_SUPPORTED;
#endif
}

static void spi_cleanup(led_strip_t *strip) {
#ifdef __linux__
    spi_backend_t *backend = (spi_backend_t *)strip->backend_data;
    if (!backend) return;

    if (backend->fd >= 0) {
        close(backend->fd);
    }
    free(backend->tx_buffer);
    free(backend);
    strip->backend_data = NULL;
#else
    UNUSED(strip);
#endif
}

static result_t spi_render(led_strip_t *strip) {
#ifdef __linux__
    spi_backend_t *backend = (spi_backend_t *)strip->backend_data;
    if (!backend) return RESULT_NOT_INITIALIZED;

    /* Encode pixel data */
    encode_pixels(strip, backend);

    /* Send via SPI */
    struct spi_ioc_transfer transfer = {
        .tx_buf = (unsigned long)backend->tx_buffer,
        .rx_buf = 0,
        .len = backend->tx_buffer_size,
        .speed_hz = backend->speed_hz,
        .delay_usecs = 0,
        .bits_per_word = 8,
    };

    if (ioctl(backend->fd, SPI_IOC_MESSAGE(1), &transfer) < 0) {
        LOG_ERROR("SPI transfer failed: %s", strerror(errno));
        return RESULT_IO_ERROR;
    }

    return RESULT_OK;
#else
    UNUSED(strip);
    return RESULT_NOT_SUPPORTED;
#endif
}

/* Backend operations structure */
const led_backend_ops_t led_spi_ops = {
    .init = spi_init,
    .cleanup = spi_cleanup,
    .render = spi_render
};

#endif /* LED_SUPPORT */
