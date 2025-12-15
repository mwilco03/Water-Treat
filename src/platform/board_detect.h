/**
 * @file board_detect.h
 * @brief Board Detection and Pre-coded Pin Configurations
 *
 * Automatically detects the SBC hardware and provides pre-coded GPIO/I2C/SPI
 * pin mappings. This minimizes user configuration and prevents errors.
 *
 * Supported Boards:
 * - Raspberry Pi (all models: 1/2/3/4/5, Zero, Zero 2)
 * - Orange Pi Zero 3 (Allwinner H618)
 * - Orange Pi Zero 2W (Allwinner H618)
 * - Luckfox Lyra Pi (Rockchip RK3506)
 *
 * Usage:
 *   board_info_t board;
 *   board_detect(&board);
 *   int gpio_pin = board.pins.gpio_relay_1;  // Use pre-mapped pin
 */

#ifndef BOARD_DETECT_H
#define BOARD_DETECT_H

#include "common.h"

/* ============================================================================
 * Board Types
 * ========================================================================== */

typedef enum {
    BOARD_TYPE_UNKNOWN = 0,

    /* Raspberry Pi Family */
    BOARD_TYPE_RPI_1,           /* BCM2835 - Original Pi */
    BOARD_TYPE_RPI_2,           /* BCM2836/7 */
    BOARD_TYPE_RPI_3,           /* BCM2837 */
    BOARD_TYPE_RPI_4,           /* BCM2711 */
    BOARD_TYPE_RPI_5,           /* BCM2712 */
    BOARD_TYPE_RPI_ZERO,        /* BCM2835 - Zero/Zero W */
    BOARD_TYPE_RPI_ZERO_2,      /* BCM2710A1 - Zero 2 W */

    /* Orange Pi Family (Allwinner) */
    BOARD_TYPE_OPI_ZERO_3,      /* Allwinner H618 */
    BOARD_TYPE_OPI_ZERO_2W,     /* Allwinner H618 */
    BOARD_TYPE_OPI_3_LTS,       /* Allwinner H6 */
    BOARD_TYPE_OPI_5,           /* Rockchip RK3588S */

    /* Luckfox Family (Rockchip) */
    BOARD_TYPE_LUCKFOX_LYRA,    /* RK3506 (Cortex-A7 + M0) */
    BOARD_TYPE_LUCKFOX_PICO,    /* RV1103/RV1106 */

    /* Generic fallback */
    BOARD_TYPE_GENERIC_ARM,
    BOARD_TYPE_GENERIC_X86,

    BOARD_TYPE_COUNT
} board_type_t;

/* ============================================================================
 * Pin Configuration Structures
 * ========================================================================== */

/**
 * GPIO pin mapping for common functions
 * Pin numbers are chip-relative (use with /dev/gpiochipN)
 */
typedef struct {
    /* I2C buses (typically for sensors like ADS1115, BME280) */
    int i2c_bus_primary;        /* Main I2C bus number (/dev/i2c-N) */
    int i2c_bus_secondary;      /* Secondary I2C bus (if available) */

    /* SPI buses (typically for ADC chips like MCP3008) */
    int spi_bus;                /* SPI bus number */
    int spi_cs0;                /* Chip select 0 */
    int spi_cs1;                /* Chip select 1 */

    /* 1-Wire (typically for DS18B20 temperature sensors) */
    int onewire_data;           /* 1-Wire data pin (GPIO) */

    /* Common output pins (relays, pumps, solenoids) */
    int gpio_relay_1;           /* Relay/output 1 */
    int gpio_relay_2;           /* Relay/output 2 */
    int gpio_relay_3;           /* Relay/output 3 */
    int gpio_relay_4;           /* Relay/output 4 */

    /* Common input pins (float switches, buttons) */
    int gpio_input_1;           /* Digital input 1 */
    int gpio_input_2;           /* Digital input 2 */
    int gpio_input_3;           /* Digital input 3 */
    int gpio_input_4;           /* Digital input 4 */

    /* Status LEDs */
    int gpio_led_status;        /* Status LED */
    int gpio_led_error;         /* Error LED */

    /* PWM channels (if available) */
    int pwm_channel_0;          /* PWM output 0 */
    int pwm_channel_1;          /* PWM output 1 */

    /* UART for external devices */
    int uart_tx;                /* UART TX */
    int uart_rx;                /* UART RX */

    /* GPIO chip name for libgpiod */
    char gpio_chip[32];         /* e.g., "gpiochip0" */

} pin_config_t;

/**
 * Board capabilities
 */
typedef struct {
    bool has_wifi;
    bool has_bluetooth;
    bool has_ethernet;
    bool has_hdmi;
    bool has_camera_csi;
    bool has_display_dsi;
    int i2c_bus_count;
    int spi_bus_count;
    int uart_count;
    int pwm_channels;
    int gpio_count;
    int ram_mb;                 /* RAM in megabytes */
    int cpu_cores;
} board_capabilities_t;

/**
 * Complete board information
 */
typedef struct {
    board_type_t type;
    char name[64];              /* Human-readable name */
    char model[128];            /* Full model string from device tree */
    char soc[32];               /* SoC name (e.g., "BCM2711") */
    char manufacturer[32];      /* Board manufacturer */

    pin_config_t pins;          /* Pre-coded pin mappings */
    board_capabilities_t caps;  /* Hardware capabilities */

    /* Detection confidence (0-100) */
    int confidence;
} board_info_t;

/* ============================================================================
 * Board Detection API
 * ========================================================================== */

/**
 * Detect the current board and populate info structure
 *
 * @param info Output structure to fill with board information
 * @return RESULT_OK on success, RESULT_NOT_FOUND if board not recognized
 */
result_t board_detect(board_info_t *info);

/**
 * Get board type from name string
 */
board_type_t board_type_from_string(const char *name);

/**
 * Get human-readable board name
 */
const char* board_type_to_string(board_type_t type);

/**
 * Get default pin configuration for a board type
 *
 * @param type Board type
 * @param pins Output pin configuration
 * @return RESULT_OK on success
 */
result_t board_get_default_pins(board_type_t type, pin_config_t *pins);

/**
 * Print board information to log
 */
void board_info_log(const board_info_t *info);

/**
 * Check if a specific feature is supported on detected board
 */
bool board_supports_i2c(const board_info_t *info);
bool board_supports_spi(const board_info_t *info);
bool board_supports_onewire(const board_info_t *info);
bool board_supports_pwm(const board_info_t *info);

#endif /* BOARD_DETECT_H */
