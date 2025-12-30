/**
 * @file main.c
 * @brief RP2040 WS2812 LED Controller for Water-Treat RTU
 *
 * Provides USB CDC serial interface for controlling WS2812 LEDs.
 * Uses PIO for cycle-accurate LED timing independent of CPU load.
 *
 * Protocol (binary):
 *   Command frame: [0x55] [CMD] [LEN] [PAYLOAD...] [CRC8]
 *   Response:      [0xAA] [STATUS] [CRC8]
 *
 * Commands:
 *   0x01 INIT       - Set LED count: [count_h] [count_l]
 *   0x02 SET_ALL    - Set all LEDs:  [R] [G] [B]
 *   0x03 SET_PIXEL  - Set one LED:   [idx_h] [idx_l] [R] [G] [B]
 *   0x04 SET_RANGE  - Set range:     [start_h] [start_l] [count_h] [count_l] [RGB...]
 *   0x05 RENDER     - Update LEDs:   (no payload)
 *   0x06 BRIGHTNESS - Set brightness: [0-255]
 *   0x07 CLEAR      - All LEDs off:  (no payload)
 *   0x08 IDENTIFY   - Flash pattern: (no payload)
 *   0xFE INFO       - Get device info
 *   0xFF PING       - Keepalive:     (no payload)
 *
 * Status codes:
 *   0x00 OK
 *   0x01 ERROR (invalid command)
 *   0x02 ERROR (invalid length)
 *   0x03 ERROR (invalid index)
 *   0x04 ERROR (CRC mismatch)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h"

/* ============================================================================
 * Configuration
 * ========================================================================== */

#define LED_PIN             15      /* GPIO pin for WS2812 data */
#define MAX_LEDS            64      /* Maximum supported LEDs */
#define DEFAULT_LED_COUNT   8       /* Default LED count */
#define DEFAULT_BRIGHTNESS  64      /* Default brightness (0-255) */
#define WATCHDOG_TIMEOUT_MS 5000    /* LEDs off if no communication */
#define WS2812_FREQ         800000  /* WS2812 bit frequency */

/* Protocol constants */
#define FRAME_START         0x55
#define RESPONSE_START      0xAA
#define MAX_PAYLOAD         (MAX_LEDS * 3 + 4)

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

/* ============================================================================
 * Global State
 * ========================================================================== */

static PIO pio;
static uint sm;
static uint offset;

static uint32_t pixels[MAX_LEDS];   /* Pixel buffer (GRB format) */
static uint16_t led_count = DEFAULT_LED_COUNT;
static uint8_t brightness = DEFAULT_BRIGHTNESS;
static uint32_t last_activity_ms = 0;
static bool leds_on = false;

/* ============================================================================
 * CRC-8 (polynomial 0x31, init 0xFF)
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
 * WS2812 Output
 * ========================================================================== */

static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(pio, sm, pixel_grb << 8u);
}

static void render_pixels(void) {
    for (uint16_t i = 0; i < led_count; i++) {
        /* Apply brightness scaling */
        uint8_t g = ((pixels[i] >> 16) & 0xFF) * brightness / 255;
        uint8_t r = ((pixels[i] >> 8) & 0xFF) * brightness / 255;
        uint8_t b = (pixels[i] & 0xFF) * brightness / 255;
        put_pixel((g << 16) | (r << 8) | b);
    }
    leds_on = true;
}

static void clear_pixels(void) {
    memset(pixels, 0, sizeof(pixels));
}

static void set_pixel(uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index < led_count) {
        pixels[index] = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    }
}

static void set_all_pixels(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t color = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    for (uint16_t i = 0; i < led_count; i++) {
        pixels[i] = color;
    }
}

/* ============================================================================
 * Identify Pattern (flash cyan rapidly)
 * ========================================================================== */

static void identify_pattern(void) {
    for (int i = 0; i < 6; i++) {
        set_all_pixels(0, 255, 255);  /* Cyan */
        render_pixels();
        sleep_ms(100);
        clear_pixels();
        render_pixels();
        sleep_ms(100);
    }
}

/* ============================================================================
 * Protocol Handling
 * ========================================================================== */

static void send_response(uint8_t status) {
    uint8_t response[3];
    response[0] = RESPONSE_START;
    response[1] = status;
    response[2] = crc8(&response[1], 1);

    for (int i = 0; i < 3; i++) {
        putchar_raw(response[i]);
    }
    stdio_flush();
}

static void send_info_response(void) {
    /* Response: [0xAA] [STATUS] [led_count_h] [led_count_l] [brightness] [id...] [CRC] */
    uint8_t response[32];
    int len = 0;

    response[len++] = RESPONSE_START;
    response[len++] = STATUS_OK;
    response[len++] = (led_count >> 8) & 0xFF;
    response[len++] = led_count & 0xFF;
    response[len++] = brightness;

    /* Add unique board ID */
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
        response[len++] = board_id.id[i];
    }

    response[len] = crc8(&response[1], len - 1);
    len++;

    for (int i = 0; i < len; i++) {
        putchar_raw(response[i]);
    }
    stdio_flush();
}

static void process_command(uint8_t cmd, const uint8_t *payload, uint8_t len) {
    last_activity_ms = to_ms_since_boot(get_absolute_time());

    switch (cmd) {
        case CMD_INIT:
            if (len != 2) {
                send_response(STATUS_ERR_LEN);
                return;
            }
            {
                uint16_t count = ((uint16_t)payload[0] << 8) | payload[1];
                if (count == 0 || count > MAX_LEDS) {
                    send_response(STATUS_ERR_INDEX);
                    return;
                }
                led_count = count;
                clear_pixels();
            }
            send_response(STATUS_OK);
            break;

        case CMD_SET_ALL:
            if (len != 3) {
                send_response(STATUS_ERR_LEN);
                return;
            }
            set_all_pixels(payload[0], payload[1], payload[2]);
            send_response(STATUS_OK);
            break;

        case CMD_SET_PIXEL:
            if (len != 5) {
                send_response(STATUS_ERR_LEN);
                return;
            }
            {
                uint16_t idx = ((uint16_t)payload[0] << 8) | payload[1];
                if (idx >= led_count) {
                    send_response(STATUS_ERR_INDEX);
                    return;
                }
                set_pixel(idx, payload[2], payload[3], payload[4]);
            }
            send_response(STATUS_OK);
            break;

        case CMD_SET_RANGE:
            if (len < 4) {
                send_response(STATUS_ERR_LEN);
                return;
            }
            {
                uint16_t start = ((uint16_t)payload[0] << 8) | payload[1];
                uint16_t count = ((uint16_t)payload[2] << 8) | payload[3];
                if (start + count > led_count || len != 4 + count * 3) {
                    send_response(STATUS_ERR_LEN);
                    return;
                }
                for (uint16_t i = 0; i < count; i++) {
                    uint16_t off = 4 + i * 3;
                    set_pixel(start + i, payload[off], payload[off + 1], payload[off + 2]);
                }
            }
            send_response(STATUS_OK);
            break;

        case CMD_RENDER:
            render_pixels();
            send_response(STATUS_OK);
            break;

        case CMD_BRIGHTNESS:
            if (len != 1) {
                send_response(STATUS_ERR_LEN);
                return;
            }
            brightness = payload[0];
            send_response(STATUS_OK);
            break;

        case CMD_CLEAR:
            clear_pixels();
            render_pixels();
            send_response(STATUS_OK);
            break;

        case CMD_IDENTIFY:
            send_response(STATUS_OK);
            identify_pattern();
            break;

        case CMD_INFO:
            send_info_response();
            break;

        case CMD_PING:
            send_response(STATUS_OK);
            break;

        default:
            send_response(STATUS_ERR_CMD);
            break;
    }
}

/* ============================================================================
 * Frame Parser State Machine
 * ========================================================================== */

typedef enum {
    STATE_WAIT_START,
    STATE_READ_CMD,
    STATE_READ_LEN,
    STATE_READ_PAYLOAD,
    STATE_READ_CRC
} parser_state_t;

static parser_state_t parser_state = STATE_WAIT_START;
static uint8_t cmd_buf;
static uint8_t len_buf;
static uint8_t payload_buf[MAX_PAYLOAD];
static uint8_t payload_idx;

static void parse_byte(uint8_t byte) {
    switch (parser_state) {
        case STATE_WAIT_START:
            if (byte == FRAME_START) {
                parser_state = STATE_READ_CMD;
            }
            break;

        case STATE_READ_CMD:
            cmd_buf = byte;
            parser_state = STATE_READ_LEN;
            break;

        case STATE_READ_LEN:
            len_buf = byte;
            if (len_buf > MAX_PAYLOAD) {
                send_response(STATUS_ERR_LEN);
                parser_state = STATE_WAIT_START;
            } else if (len_buf == 0) {
                parser_state = STATE_READ_CRC;
            } else {
                payload_idx = 0;
                parser_state = STATE_READ_PAYLOAD;
            }
            break;

        case STATE_READ_PAYLOAD:
            payload_buf[payload_idx++] = byte;
            if (payload_idx >= len_buf) {
                parser_state = STATE_READ_CRC;
            }
            break;

        case STATE_READ_CRC:
            {
                /* Calculate expected CRC over cmd + len + payload */
                uint8_t check_buf[MAX_PAYLOAD + 2];
                check_buf[0] = cmd_buf;
                check_buf[1] = len_buf;
                memcpy(&check_buf[2], payload_buf, len_buf);
                uint8_t expected_crc = crc8(check_buf, 2 + len_buf);

                if (byte != expected_crc) {
                    send_response(STATUS_ERR_CRC);
                } else {
                    process_command(cmd_buf, payload_buf, len_buf);
                }
            }
            parser_state = STATE_WAIT_START;
            break;
    }
}

/* ============================================================================
 * Watchdog - Turn off LEDs if no communication
 * ========================================================================== */

static void check_watchdog(void) {
    if (!leds_on) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_activity_ms > WATCHDOG_TIMEOUT_MS) {
        /* No communication - turn off LEDs for safety */
        clear_pixels();
        render_pixels();
        leds_on = false;
    }
}

/* ============================================================================
 * Main
 * ========================================================================== */

int main(void) {
    /* Initialize stdio (USB CDC) */
    stdio_init_all();

    /* Initialize PIO for WS2812 */
    pio = pio0;
    sm = 0;
    offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, LED_PIN, WS2812_FREQ, false);

    /* Clear LEDs on startup */
    clear_pixels();
    render_pixels();

    /* Brief startup indication */
    set_all_pixels(0, 32, 0);  /* Dim green */
    render_pixels();
    sleep_ms(200);
    clear_pixels();
    render_pixels();

    last_activity_ms = to_ms_since_boot(get_absolute_time());

    /* Main loop */
    while (true) {
        /* Check for incoming data */
        int c = getchar_timeout_us(10000);  /* 10ms timeout */
        if (c != PICO_ERROR_TIMEOUT) {
            parse_byte((uint8_t)c);
        }

        /* Check watchdog */
        check_watchdog();
    }

    return 0;
}
