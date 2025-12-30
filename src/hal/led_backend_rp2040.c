/**
 * @file led_backend_rp2040.c
 * @brief RP2040 USB backend for WS2812 LEDs
 *
 * Communicates with an RP2040 microcontroller running the LED controller
 * firmware over USB CDC serial. The RP2040 handles WS2812 timing using
 * PIO, providing platform-agnostic LED control.
 *
 * Benefits:
 * - Works on any SBC with USB (no SPI/GPIO config needed)
 * - Perfect WS2812 timing via RP2040 PIO
 * - Standalone module with watchdog safety
 * - Hot-pluggable with udev device detection
 */

#include "led_ws2812.h"

#ifdef LED_SUPPORT

#include "utils/logger.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <glob.h>
#include <sys/select.h>

#ifdef __linux__
#include <termios.h>
#endif

/* Protocol constants (must match firmware) */
#define FRAME_START         0x55
#define RESPONSE_START      0xAA
#define RESPONSE_TIMEOUT_MS 100

/* Commands */
#define CMD_INIT            0x01
#define CMD_SET_ALL         0x02
#define CMD_SET_PIXEL       0x03
#define CMD_SET_RANGE       0x04
#define CMD_RENDER          0x05
#define CMD_BRIGHTNESS      0x06
#define CMD_CLEAR           0x07
#define CMD_IDENTIFY        0x08
#define CMD_INFO            0xFE
#define CMD_PING            0xFF

/* Status codes */
#define STATUS_OK           0x00
#define STATUS_ERR_CMD      0x01
#define STATUS_ERR_LEN      0x02
#define STATUS_ERR_INDEX    0x03
#define STATUS_ERR_CRC      0x04

/* RP2040 backend state */
typedef struct {
    int fd;                     /* Serial port file descriptor */
    char device_path[128];      /* Device path */
    uint8_t *tx_buffer;         /* Transmission buffer */
    size_t tx_buffer_size;
    bool connected;
} rp2040_backend_t;

/* ============================================================================
 * CRC-8 (polynomial 0x31, init 0xFF) - must match firmware
 * ========================================================================== */

static uint8_t crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ============================================================================
 * Serial Port Helpers
 * ========================================================================== */

#ifdef __linux__
static result_t configure_serial(int fd, speed_t baud) {
    struct termios tty;

    if (tcgetattr(fd, &tty) != 0) {
        LOG_ERROR("tcgetattr failed: %s", strerror(errno));
        return RESULT_IO_ERROR;
    }

    /* Set baud rate */
    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);

    /* 8N1, no flow control */
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;

    /* Raw mode */
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;

    /* Read timeouts */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;  /* 100ms timeout */

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        LOG_ERROR("tcsetattr failed: %s", strerror(errno));
        return RESULT_IO_ERROR;
    }

    /* Flush any pending data */
    tcflush(fd, TCIOFLUSH);

    return RESULT_OK;
}
#endif

static result_t find_rp2040_device(char *path, size_t path_size) {
    /* Look for the LED controller by vendor string */
    glob_t glob_result;
    const char *patterns[] = {
        "/dev/serial/by-id/*WaterTreat*LED*",
        "/dev/serial/by-id/*Raspberry_Pi_Pico*",
        "/dev/ttyACM*",
        NULL
    };

    for (int i = 0; patterns[i] != NULL; i++) {
        if (glob(patterns[i], GLOB_NOSORT, NULL, &glob_result) == 0) {
            if (glob_result.gl_pathc > 0) {
                strncpy(path, glob_result.gl_pathv[0], path_size - 1);
                path[path_size - 1] = '\0';
                globfree(&glob_result);
                return RESULT_OK;
            }
            globfree(&glob_result);
        }
    }

    return RESULT_NOT_FOUND;
}

/* ============================================================================
 * Protocol Communication
 * ========================================================================== */

static result_t send_command(rp2040_backend_t *backend, uint8_t cmd,
                              const uint8_t *payload, uint8_t payload_len) {
    if (!backend || backend->fd < 0) {
        return RESULT_NOT_INITIALIZED;
    }

    /* Build frame: [START] [CMD] [LEN] [PAYLOAD...] [CRC] */
    uint8_t frame[256 + 4];
    size_t frame_len = 0;

    frame[frame_len++] = FRAME_START;
    frame[frame_len++] = cmd;
    frame[frame_len++] = payload_len;

    if (payload_len > 0 && payload != NULL) {
        memcpy(&frame[frame_len], payload, payload_len);
        frame_len += payload_len;
    }

    /* CRC over cmd + len + payload */
    uint8_t crc = crc8(&frame[1], 2 + payload_len);
    frame[frame_len++] = crc;

    /* Send frame */
    ssize_t written = write(backend->fd, frame, frame_len);
    if (written != (ssize_t)frame_len) {
        LOG_ERROR("Write failed: %s", strerror(errno));
        return RESULT_IO_ERROR;
    }

    return RESULT_OK;
}

static result_t read_response(rp2040_backend_t *backend, uint8_t *status) {
    if (!backend || backend->fd < 0) {
        return RESULT_NOT_INITIALIZED;
    }

    /* Wait for response with timeout */
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(backend->fd, &readfds);
    tv.tv_sec = 0;
    tv.tv_usec = RESPONSE_TIMEOUT_MS * 1000;

    int ret = select(backend->fd + 1, &readfds, NULL, NULL, &tv);
    if (ret <= 0) {
        LOG_DEBUG("Response timeout");
        return RESULT_TIMEOUT;
    }

    /* Read response: [0xAA] [STATUS] [CRC] */
    uint8_t response[3];
    ssize_t n = read(backend->fd, response, 3);
    if (n != 3) {
        LOG_DEBUG("Short response: %zd bytes", n);
        return RESULT_IO_ERROR;
    }

    if (response[0] != RESPONSE_START) {
        LOG_DEBUG("Invalid response start: 0x%02X", response[0]);
        return RESULT_IO_ERROR;
    }

    /* Verify CRC */
    uint8_t expected_crc = crc8(&response[1], 1);
    if (response[2] != expected_crc) {
        LOG_DEBUG("Response CRC mismatch");
        return RESULT_IO_ERROR;
    }

    if (status) {
        *status = response[1];
    }

    return (response[1] == STATUS_OK) ? RESULT_OK : RESULT_ERROR;
}

static result_t send_and_wait(rp2040_backend_t *backend, uint8_t cmd,
                               const uint8_t *payload, uint8_t payload_len) {
    result_t result = send_command(backend, cmd, payload, payload_len);
    if (result != RESULT_OK) {
        return result;
    }

    uint8_t status;
    result = read_response(backend, &status);
    if (result != RESULT_OK) {
        LOG_DEBUG("Command 0x%02X failed: status=0x%02X", cmd, status);
    }

    return result;
}

/* ============================================================================
 * Backend Operations
 * ========================================================================== */

static result_t rp2040_init(led_strip_t *strip, const led_config_t *config) {
#ifdef __linux__
    rp2040_backend_t *backend = calloc(1, sizeof(rp2040_backend_t));
    if (!backend) {
        return RESULT_NO_MEMORY;
    }

    backend->fd = -1;

    /* Find device path */
    if (config->rp2040_device[0] != '\0') {
        strncpy(backend->device_path, config->rp2040_device,
                sizeof(backend->device_path) - 1);
    } else {
        result_t result = find_rp2040_device(backend->device_path,
                                              sizeof(backend->device_path));
        if (result != RESULT_OK) {
            LOG_ERROR("RP2040 LED controller not found");
            free(backend);
            return RESULT_NOT_FOUND;
        }
    }

    LOG_DEBUG("Opening RP2040 LED controller at %s", backend->device_path);

    /* Open serial port */
    backend->fd = open(backend->device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (backend->fd < 0) {
        LOG_ERROR("Failed to open %s: %s", backend->device_path, strerror(errno));
        free(backend);
        return RESULT_IO_ERROR;
    }

    /* Clear non-blocking after open */
    int flags = fcntl(backend->fd, F_GETFL, 0);
    fcntl(backend->fd, F_SETFL, flags & ~O_NONBLOCK);

    /* Configure serial port */
    speed_t baud = B115200;
    if (config->rp2040_baud == 9600) baud = B9600;
    else if (config->rp2040_baud == 19200) baud = B19200;
    else if (config->rp2040_baud == 38400) baud = B38400;
    else if (config->rp2040_baud == 57600) baud = B57600;
    else if (config->rp2040_baud == 230400) baud = B230400;

    result_t result = configure_serial(backend->fd, baud);
    if (result != RESULT_OK) {
        close(backend->fd);
        free(backend);
        return result;
    }

    /* Small delay for USB CDC to settle */
    usleep(50000);

    /* Send INIT command with LED count */
    uint8_t init_payload[2] = {
        (strip->led_count >> 8) & 0xFF,
        strip->led_count & 0xFF
    };
    result = send_and_wait(backend, CMD_INIT, init_payload, 2);
    if (result != RESULT_OK) {
        LOG_WARN("INIT command failed, device may need reset");
        /* Continue anyway - device might already be configured */
    }

    /* Set brightness */
    uint8_t brightness = strip->brightness;
    send_and_wait(backend, CMD_BRIGHTNESS, &brightness, 1);

    backend->connected = true;
    strip->backend_data = backend;

    LOG_INFO("RP2040 LED backend initialized: device=%s, leds=%d, brightness=%d",
             backend->device_path, strip->led_count, strip->brightness);

    return RESULT_OK;
#else
    UNUSED(strip);
    UNUSED(config);
    LOG_ERROR("RP2040 backend requires Linux");
    return RESULT_NOT_SUPPORTED;
#endif
}

static void rp2040_cleanup(led_strip_t *strip) {
    rp2040_backend_t *backend = (rp2040_backend_t *)strip->backend_data;
    if (!backend) return;

    /* Turn off LEDs before closing */
    if (backend->fd >= 0 && backend->connected) {
        send_and_wait(backend, CMD_CLEAR, NULL, 0);
    }

    if (backend->fd >= 0) {
        close(backend->fd);
    }

    free(backend);
    strip->backend_data = NULL;

    LOG_DEBUG("RP2040 LED backend cleaned up");
}

static result_t rp2040_render(led_strip_t *strip) {
    rp2040_backend_t *backend = (rp2040_backend_t *)strip->backend_data;
    if (!backend || backend->fd < 0) {
        return RESULT_NOT_INITIALIZED;
    }

    /* Send all pixel data using SET_RANGE command */
    /* Payload: [start_h] [start_l] [count_h] [count_l] [R G B ...] */
    size_t payload_size = 4 + (strip->led_count * 3);
    uint8_t *payload = alloca(payload_size);

    payload[0] = 0;  /* start high byte */
    payload[1] = 0;  /* start low byte */
    payload[2] = (strip->led_count >> 8) & 0xFF;
    payload[3] = strip->led_count & 0xFF;

    /* Copy pixel data (RGB order) */
    for (uint16_t i = 0; i < strip->led_count; i++) {
        payload[4 + i * 3 + 0] = strip->pixels[i].r;
        payload[4 + i * 3 + 1] = strip->pixels[i].g;
        payload[4 + i * 3 + 2] = strip->pixels[i].b;
    }

    result_t result = send_and_wait(backend, CMD_SET_RANGE, payload, payload_size);
    if (result != RESULT_OK) {
        return result;
    }

    /* Send RENDER command to update LEDs */
    return send_and_wait(backend, CMD_RENDER, NULL, 0);
}

/* Backend operations structure */
const led_backend_ops_t led_rp2040_ops = {
    .init = rp2040_init,
    .cleanup = rp2040_cleanup,
    .render = rp2040_render
};

#endif /* LED_SUPPORT */
