/**
 * @file board_detect.c
 * @brief Board Detection and Pre-coded Pin Configurations
 *
 * Detects SBC hardware via:
 * 1. /proc/device-tree/model (standard on ARM Linux)
 * 2. /proc/cpuinfo (fallback)
 * 3. /etc/armbian-release (Armbian-specific)
 * 4. /sys/firmware/devicetree/base/compatible
 */

#include "board_detect.h"
#include "utils/logger.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* ============================================================================
 * Pre-coded Pin Configurations
 *
 * These are the default GPIO pin mappings for each supported board.
 * Pin numbers are chip-relative (for use with libgpiod).
 * ========================================================================== */

/* Raspberry Pi (all models use BCM GPIO numbering) */
static const pin_config_t RPI_PINS = {
    .i2c_bus_primary = 1,       /* /dev/i2c-1 (GPIO 2=SDA, 3=SCL) */
    .i2c_bus_secondary = 0,     /* /dev/i2c-0 (reserved on some models) */
    .spi_bus = 0,               /* /dev/spidev0.x */
    .spi_cs0 = 8,               /* CE0 = GPIO 8 */
    .spi_cs1 = 7,               /* CE1 = GPIO 7 */
    .onewire_data = 4,          /* GPIO 4 (default 1-Wire) */
    .gpio_relay_1 = 17,         /* GPIO 17 */
    .gpio_relay_2 = 27,         /* GPIO 27 */
    .gpio_relay_3 = 22,         /* GPIO 22 */
    .gpio_relay_4 = 23,         /* GPIO 23 */
    .gpio_input_1 = 5,          /* GPIO 5 */
    .gpio_input_2 = 6,          /* GPIO 6 */
    .gpio_input_3 = 13,         /* GPIO 13 */
    .gpio_input_4 = 19,         /* GPIO 19 */
    .gpio_led_status = 16,      /* GPIO 16 */
    .gpio_led_error = 20,       /* GPIO 20 */
    .pwm_channel_0 = 18,        /* GPIO 18 (PWM0) */
    .pwm_channel_1 = 19,        /* GPIO 19 (PWM1) - shared with input */
    .uart_tx = 14,              /* GPIO 14 (TXD) */
    .uart_rx = 15,              /* GPIO 15 (RXD) */
    .gpio_chip = "gpiochip0",   /* BCM GPIO chip */
};

/* Raspberry Pi 5 (uses gpiochip4 for GPIO header) */
static const pin_config_t RPI5_PINS = {
    .i2c_bus_primary = 1,
    .i2c_bus_secondary = 0,
    .spi_bus = 0,
    .spi_cs0 = 8,
    .spi_cs1 = 7,
    .onewire_data = 4,
    .gpio_relay_1 = 17,
    .gpio_relay_2 = 27,
    .gpio_relay_3 = 22,
    .gpio_relay_4 = 23,
    .gpio_input_1 = 5,
    .gpio_input_2 = 6,
    .gpio_input_3 = 13,
    .gpio_input_4 = 19,
    .gpio_led_status = 16,
    .gpio_led_error = 20,
    .pwm_channel_0 = 18,
    .pwm_channel_1 = 19,
    .uart_tx = 14,
    .uart_rx = 15,
    .gpio_chip = "gpiochip4",   /* RPi 5 uses gpiochip4 for header GPIOs */
};

/*
 * Orange Pi Zero 3 / Zero 2W (Allwinner H618)
 *
 * H618 GPIO naming: Port Letter + Pin Number
 * - PA0 = 0, PA1 = 1, ..., PA31 = 31
 * - PC0 = 64, PC1 = 65, ...
 * - PG0 = 192, ...
 * - PH0 = 224, ...
 * - PI0 = 256, ...
 *
 * Formula: (port_letter - 'A') * 32 + pin_number
 */
#define H618_GPIO(port, pin) (((port) - 'A') * 32 + (pin))

static const pin_config_t OPI_ZERO3_PINS = {
    .i2c_bus_primary = 3,       /* /dev/i2c-3 (PI5=SDA, PI6=SCL) */
    .i2c_bus_secondary = 4,     /* /dev/i2c-4 (PI7=SDA, PI8=SCL) */
    .spi_bus = 1,               /* /dev/spidev1.x */
    .spi_cs0 = H618_GPIO('H', 9),   /* PH9 = SPI1_CS0 = 233 */
    .spi_cs1 = -1,              /* Not available */
    .onewire_data = H618_GPIO('I', 0),  /* PI0 = 256 (suggested) */
    .gpio_relay_1 = H618_GPIO('I', 1),  /* PI1 = 257 */
    .gpio_relay_2 = H618_GPIO('I', 2),  /* PI2 = 258 */
    .gpio_relay_3 = H618_GPIO('I', 3),  /* PI3 = 259 */
    .gpio_relay_4 = H618_GPIO('I', 4),  /* PI4 = 260 */
    .gpio_input_1 = H618_GPIO('I', 9),  /* PI9 = 265 */
    .gpio_input_2 = H618_GPIO('I', 10), /* PI10 = 266 */
    .gpio_input_3 = H618_GPIO('I', 11), /* PI11 = 267 */
    .gpio_input_4 = H618_GPIO('I', 12), /* PI12 = 268 */
    .gpio_led_status = H618_GPIO('I', 15), /* PI15 = 271 */
    .gpio_led_error = H618_GPIO('I', 16),  /* PI16 = 272 */
    .pwm_channel_0 = H618_GPIO('H', 0),    /* PH0 = 224 (PWM available) */
    .pwm_channel_1 = -1,
    .uart_tx = H618_GPIO('H', 0),   /* UART0_TX = PH0 */
    .uart_rx = H618_GPIO('H', 1),   /* UART0_RX = PH1 */
    .gpio_chip = "gpiochip0",
};

/* Orange Pi Zero 2W (same SoC as Zero 3, slightly different pinout) */
static const pin_config_t OPI_ZERO2W_PINS = {
    .i2c_bus_primary = 3,
    .i2c_bus_secondary = 4,
    .spi_bus = 1,
    .spi_cs0 = H618_GPIO('H', 9),
    .spi_cs1 = -1,
    .onewire_data = H618_GPIO('I', 0),
    .gpio_relay_1 = H618_GPIO('I', 1),
    .gpio_relay_2 = H618_GPIO('I', 2),
    .gpio_relay_3 = H618_GPIO('I', 3),
    .gpio_relay_4 = H618_GPIO('I', 4),
    .gpio_input_1 = H618_GPIO('I', 9),
    .gpio_input_2 = H618_GPIO('I', 10),
    .gpio_input_3 = H618_GPIO('I', 11),
    .gpio_input_4 = H618_GPIO('I', 12),
    .gpio_led_status = H618_GPIO('I', 15),
    .gpio_led_error = H618_GPIO('I', 16),
    .pwm_channel_0 = H618_GPIO('H', 0),
    .pwm_channel_1 = -1,
    .uart_tx = H618_GPIO('H', 0),
    .uart_rx = H618_GPIO('H', 1),
    .gpio_chip = "gpiochip0",
};

/*
 * Luckfox Lyra Pi (Rockchip RK3506)
 *
 * RK3506 GPIO naming: GPIO bank + pin
 * - GPIO0_A0 = 0, GPIO0_A1 = 1, ..., GPIO0_A7 = 7
 * - GPIO0_B0 = 8, ..., GPIO0_B7 = 15
 * - GPIO0_C0 = 16, ..., GPIO0_D7 = 31
 * - GPIO1_A0 = 32, ...
 *
 * Formula: bank * 32 + (sub_bank * 8) + pin
 * Where sub_bank: A=0, B=1, C=2, D=3
 */
#define RK_GPIO(bank, sub, pin) ((bank) * 32 + ((sub) * 8) + (pin))
#define RK_A 0
#define RK_B 1
#define RK_C 2
#define RK_D 3

static const pin_config_t LUCKFOX_LYRA_PINS = {
    .i2c_bus_primary = 1,       /* /dev/i2c-1 */
    .i2c_bus_secondary = 2,     /* /dev/i2c-2 */
    .spi_bus = 0,               /* /dev/spidev0.x */
    .spi_cs0 = RK_GPIO(1, RK_A, 4),  /* GPIO1_A4 = 36 */
    .spi_cs1 = -1,
    .onewire_data = RK_GPIO(1, RK_B, 0),  /* GPIO1_B0 = 40 */
    .gpio_relay_1 = RK_GPIO(1, RK_B, 1),  /* GPIO1_B1 = 41 */
    .gpio_relay_2 = RK_GPIO(1, RK_B, 2),  /* GPIO1_B2 = 42 */
    .gpio_relay_3 = RK_GPIO(1, RK_B, 3),  /* GPIO1_B3 = 43 */
    .gpio_relay_4 = RK_GPIO(1, RK_B, 4),  /* GPIO1_B4 = 44 */
    .gpio_input_1 = RK_GPIO(1, RK_C, 0),  /* GPIO1_C0 = 48 */
    .gpio_input_2 = RK_GPIO(1, RK_C, 1),  /* GPIO1_C1 = 49 */
    .gpio_input_3 = RK_GPIO(1, RK_C, 2),  /* GPIO1_C2 = 50 */
    .gpio_input_4 = RK_GPIO(1, RK_C, 3),  /* GPIO1_C3 = 51 */
    .gpio_led_status = RK_GPIO(0, RK_A, 0),  /* Onboard LED if available */
    .gpio_led_error = RK_GPIO(0, RK_A, 1),
    .pwm_channel_0 = RK_GPIO(1, RK_A, 0),
    .pwm_channel_1 = -1,
    .uart_tx = RK_GPIO(0, RK_B, 4),  /* UART2_TX */
    .uart_rx = RK_GPIO(0, RK_B, 5),  /* UART2_RX */
    .gpio_chip = "gpiochip0",
};

/* Generic fallback configuration */
static const pin_config_t GENERIC_PINS = {
    .i2c_bus_primary = 1,
    .i2c_bus_secondary = -1,
    .spi_bus = 0,
    .spi_cs0 = -1,
    .spi_cs1 = -1,
    .onewire_data = -1,
    .gpio_relay_1 = -1,
    .gpio_relay_2 = -1,
    .gpio_relay_3 = -1,
    .gpio_relay_4 = -1,
    .gpio_input_1 = -1,
    .gpio_input_2 = -1,
    .gpio_input_3 = -1,
    .gpio_input_4 = -1,
    .gpio_led_status = -1,
    .gpio_led_error = -1,
    .pwm_channel_0 = -1,
    .pwm_channel_1 = -1,
    .uart_tx = -1,
    .uart_rx = -1,
    .gpio_chip = "gpiochip0",
};

/* ============================================================================
 * Board Name Mappings
 * ========================================================================== */

static const struct {
    board_type_t type;
    const char *name;
    const char *match_strings[8];  /* Strings to search for in model */
} board_match_table[] = {
    /* Raspberry Pi */
    {BOARD_TYPE_RPI_5, "Raspberry Pi 5",
     {"Raspberry Pi 5", "BCM2712", NULL}},
    {BOARD_TYPE_RPI_4, "Raspberry Pi 4",
     {"Raspberry Pi 4", "BCM2711", NULL}},
    {BOARD_TYPE_RPI_3, "Raspberry Pi 3",
     {"Raspberry Pi 3", "BCM2837", NULL}},
    {BOARD_TYPE_RPI_2, "Raspberry Pi 2",
     {"Raspberry Pi 2", "BCM2836", "BCM2837", NULL}},
    {BOARD_TYPE_RPI_ZERO_2, "Raspberry Pi Zero 2",
     {"Raspberry Pi Zero 2", "BCM2710A1", NULL}},
    {BOARD_TYPE_RPI_ZERO, "Raspberry Pi Zero",
     {"Raspberry Pi Zero", NULL}},
    {BOARD_TYPE_RPI_1, "Raspberry Pi 1",
     {"Raspberry Pi Model", "BCM2835", NULL}},

    /* Orange Pi */
    {BOARD_TYPE_OPI_ZERO_3, "Orange Pi Zero 3",
     {"Orange Pi Zero3", "OrangePi Zero3", "sun50iw9", NULL}},
    {BOARD_TYPE_OPI_ZERO_2W, "Orange Pi Zero 2W",
     {"Orange Pi Zero 2W", "OrangePi Zero2W", "sun50iw9", NULL}},
    {BOARD_TYPE_OPI_3_LTS, "Orange Pi 3 LTS",
     {"Orange Pi 3 LTS", "OrangePi 3", NULL}},
    {BOARD_TYPE_OPI_5, "Orange Pi 5",
     {"Orange Pi 5", "OrangePi 5", "rk3588", NULL}},

    /* Luckfox */
    {BOARD_TYPE_LUCKFOX_LYRA, "Luckfox Lyra Pi",
     {"Luckfox Lyra", "RK3506", "rk3506", NULL}},
    {BOARD_TYPE_LUCKFOX_PICO, "Luckfox Pico",
     {"Luckfox Pico", "RV1103", "RV1106", NULL}},

    /* End marker */
    {BOARD_TYPE_UNKNOWN, NULL, {NULL}}
};

/* ============================================================================
 * Internal Helper Functions
 * ========================================================================== */

static bool read_file_string(const char *path, char *buf, size_t len) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    if (fgets(buf, len, fp) == NULL) {
        fclose(fp);
        return false;
    }

    /* Remove trailing newline/null */
    size_t slen = strlen(buf);
    while (slen > 0 && (buf[slen-1] == '\n' || buf[slen-1] == '\0')) {
        buf[--slen] = '\0';
    }

    fclose(fp);
    return true;
}

static bool string_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return false;

    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i+j]) !=
                tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static board_type_t detect_from_model(const char *model) {
    for (int i = 0; board_match_table[i].name != NULL; i++) {
        for (int j = 0; board_match_table[i].match_strings[j] != NULL; j++) {
            if (string_contains_ci(model, board_match_table[i].match_strings[j])) {
                return board_match_table[i].type;
            }
        }
    }
    return BOARD_TYPE_UNKNOWN;
}

static void set_board_capabilities(board_info_t *info) {
    switch (info->type) {
        case BOARD_TYPE_RPI_5:
            info->caps = (board_capabilities_t){
                .has_wifi = true, .has_bluetooth = true, .has_ethernet = true,
                .has_hdmi = true, .has_camera_csi = true, .has_display_dsi = true,
                .i2c_bus_count = 6, .spi_bus_count = 2, .uart_count = 6,
                .pwm_channels = 4, .gpio_count = 28, .ram_mb = 8192, .cpu_cores = 4
            };
            SAFE_STRNCPY(info->soc, "BCM2712", sizeof(info->soc));
            break;

        case BOARD_TYPE_RPI_4:
            info->caps = (board_capabilities_t){
                .has_wifi = true, .has_bluetooth = true, .has_ethernet = true,
                .has_hdmi = true, .has_camera_csi = true, .has_display_dsi = true,
                .i2c_bus_count = 6, .spi_bus_count = 2, .uart_count = 6,
                .pwm_channels = 2, .gpio_count = 28, .ram_mb = 4096, .cpu_cores = 4
            };
            SAFE_STRNCPY(info->soc, "BCM2711", sizeof(info->soc));
            break;

        case BOARD_TYPE_RPI_3:
        case BOARD_TYPE_RPI_ZERO_2:
            info->caps = (board_capabilities_t){
                .has_wifi = true, .has_bluetooth = true, .has_ethernet = (info->type == BOARD_TYPE_RPI_3),
                .has_hdmi = true, .has_camera_csi = true, .has_display_dsi = true,
                .i2c_bus_count = 2, .spi_bus_count = 1, .uart_count = 2,
                .pwm_channels = 2, .gpio_count = 28, .ram_mb = 1024, .cpu_cores = 4
            };
            SAFE_STRNCPY(info->soc, "BCM2837", sizeof(info->soc));
            break;

        case BOARD_TYPE_OPI_ZERO_3:
        case BOARD_TYPE_OPI_ZERO_2W:
            info->caps = (board_capabilities_t){
                .has_wifi = true, .has_bluetooth = true, .has_ethernet = true,
                .has_hdmi = true, .has_camera_csi = false, .has_display_dsi = false,
                .i2c_bus_count = 4, .spi_bus_count = 2, .uart_count = 6,
                .pwm_channels = 2, .gpio_count = 32, .ram_mb = 4096, .cpu_cores = 4
            };
            SAFE_STRNCPY(info->soc, "H618", sizeof(info->soc));
            SAFE_STRNCPY(info->manufacturer, "Xunlong", sizeof(info->manufacturer));
            break;

        case BOARD_TYPE_LUCKFOX_LYRA:
            info->caps = (board_capabilities_t){
                .has_wifi = false, .has_bluetooth = false, .has_ethernet = true,
                .has_hdmi = false, .has_camera_csi = true, .has_display_dsi = false,
                .i2c_bus_count = 3, .spi_bus_count = 2, .uart_count = 4,
                .pwm_channels = 4, .gpio_count = 48, .ram_mb = 256, .cpu_cores = 3
            };
            SAFE_STRNCPY(info->soc, "RK3506", sizeof(info->soc));
            SAFE_STRNCPY(info->manufacturer, "Luckfox", sizeof(info->manufacturer));
            break;

        default:
            info->caps = (board_capabilities_t){
                .has_wifi = false, .has_bluetooth = false, .has_ethernet = true,
                .has_hdmi = false, .has_camera_csi = false, .has_display_dsi = false,
                .i2c_bus_count = 1, .spi_bus_count = 1, .uart_count = 1,
                .pwm_channels = 0, .gpio_count = 0, .ram_mb = 0, .cpu_cores = 0
            };
            break;
    }

    /* Set manufacturer for RPi */
    if (info->type >= BOARD_TYPE_RPI_1 && info->type <= BOARD_TYPE_RPI_ZERO_2) {
        SAFE_STRNCPY(info->manufacturer, "Raspberry Pi Foundation", sizeof(info->manufacturer));
    }
}

/* ============================================================================
 * Public API Implementation
 * ========================================================================== */

result_t board_detect(board_info_t *info) {
    CHECK_NULL(info);
    memset(info, 0, sizeof(*info));
    info->type = BOARD_TYPE_UNKNOWN;
    info->confidence = 0;

    char model[256] = {0};
    char compatible[512] = {0};

    /* Method 1: Read /proc/device-tree/model (most reliable) */
    if (read_file_string("/proc/device-tree/model", model, sizeof(model))) {
        SAFE_STRNCPY(info->model, model, sizeof(info->model));
        info->type = detect_from_model(model);
        if (info->type != BOARD_TYPE_UNKNOWN) {
            info->confidence = 95;
        }
    }

    /* Method 2: Read compatible string (backup) */
    if (info->type == BOARD_TYPE_UNKNOWN) {
        if (read_file_string("/sys/firmware/devicetree/base/compatible",
                            compatible, sizeof(compatible))) {
            info->type = detect_from_model(compatible);
            if (info->type != BOARD_TYPE_UNKNOWN) {
                info->confidence = 85;
            }
        }
    }

    /* Method 3: Check for Armbian release file */
    if (info->type == BOARD_TYPE_UNKNOWN) {
        char armbian_board[128] = {0};
        if (read_file_string("/etc/armbian-release", armbian_board, sizeof(armbian_board))) {
            /* Parse BOARD= line */
            char *board_line = strstr(armbian_board, "BOARD=");
            if (board_line) {
                info->type = detect_from_model(board_line);
                if (info->type != BOARD_TYPE_UNKNOWN) {
                    info->confidence = 80;
                }
            }
        }
    }

    /* Method 4: Check architecture as last resort */
    if (info->type == BOARD_TYPE_UNKNOWN) {
#if defined(__aarch64__) || defined(__arm__)
        info->type = BOARD_TYPE_GENERIC_ARM;
        info->confidence = 30;
#elif defined(__x86_64__) || defined(__i386__)
        info->type = BOARD_TYPE_GENERIC_X86;
        info->confidence = 30;
#endif
    }

    /* Set board name */
    const char *name = board_type_to_string(info->type);
    SAFE_STRNCPY(info->name, name, sizeof(info->name));

    /* Load pin configuration */
    board_get_default_pins(info->type, &info->pins);

    /* Set capabilities */
    set_board_capabilities(info);

    LOG_INFO("Board detected: %s (confidence: %d%%)", info->name, info->confidence);

    return (info->type != BOARD_TYPE_UNKNOWN) ? RESULT_OK : RESULT_NOT_FOUND;
}

const char* board_type_to_string(board_type_t type) {
    for (int i = 0; board_match_table[i].name != NULL; i++) {
        if (board_match_table[i].type == type) {
            return board_match_table[i].name;
        }
    }

    switch (type) {
        case BOARD_TYPE_GENERIC_ARM: return "Generic ARM Board";
        case BOARD_TYPE_GENERIC_X86: return "Generic x86 System";
        default: return "Unknown Board";
    }
}

board_type_t board_type_from_string(const char *name) {
    if (!name) return BOARD_TYPE_UNKNOWN;

    for (int i = 0; board_match_table[i].name != NULL; i++) {
        if (strcasecmp(name, board_match_table[i].name) == 0) {
            return board_match_table[i].type;
        }
    }

    return BOARD_TYPE_UNKNOWN;
}

result_t board_get_default_pins(board_type_t type, pin_config_t *pins) {
    CHECK_NULL(pins);

    switch (type) {
        case BOARD_TYPE_RPI_5:
            memcpy(pins, &RPI5_PINS, sizeof(pin_config_t));
            break;

        case BOARD_TYPE_RPI_1:
        case BOARD_TYPE_RPI_2:
        case BOARD_TYPE_RPI_3:
        case BOARD_TYPE_RPI_4:
        case BOARD_TYPE_RPI_ZERO:
        case BOARD_TYPE_RPI_ZERO_2:
            memcpy(pins, &RPI_PINS, sizeof(pin_config_t));
            break;

        case BOARD_TYPE_OPI_ZERO_3:
            memcpy(pins, &OPI_ZERO3_PINS, sizeof(pin_config_t));
            break;

        case BOARD_TYPE_OPI_ZERO_2W:
            memcpy(pins, &OPI_ZERO2W_PINS, sizeof(pin_config_t));
            break;

        case BOARD_TYPE_LUCKFOX_LYRA:
        case BOARD_TYPE_LUCKFOX_PICO:
            memcpy(pins, &LUCKFOX_LYRA_PINS, sizeof(pin_config_t));
            break;

        default:
            memcpy(pins, &GENERIC_PINS, sizeof(pin_config_t));
            break;
    }

    return RESULT_OK;
}

void board_info_log(const board_info_t *info) {
    if (!info) return;

    LOG_INFO("=== Board Information ===");
    LOG_INFO("  Type: %s", info->name);
    LOG_INFO("  Model: %s", info->model);
    LOG_INFO("  SoC: %s", info->soc);
    LOG_INFO("  Manufacturer: %s", info->manufacturer);
    LOG_INFO("  Detection confidence: %d%%", info->confidence);
    LOG_INFO("  Capabilities:");
    LOG_INFO("    - WiFi: %s", info->caps.has_wifi ? "Yes" : "No");
    LOG_INFO("    - Ethernet: %s", info->caps.has_ethernet ? "Yes" : "No");
    LOG_INFO("    - I2C buses: %d", info->caps.i2c_bus_count);
    LOG_INFO("    - SPI buses: %d", info->caps.spi_bus_count);
    LOG_INFO("    - PWM channels: %d", info->caps.pwm_channels);
    LOG_INFO("    - GPIO count: %d", info->caps.gpio_count);
    LOG_INFO("  Pin Configuration:");
    LOG_INFO("    - GPIO chip: %s", info->pins.gpio_chip);
    LOG_INFO("    - I2C bus: %d", info->pins.i2c_bus_primary);
    LOG_INFO("    - SPI bus: %d", info->pins.spi_bus);
    LOG_INFO("    - 1-Wire pin: %d", info->pins.onewire_data);
    LOG_INFO("    - Relay pins: %d, %d, %d, %d",
             info->pins.gpio_relay_1, info->pins.gpio_relay_2,
             info->pins.gpio_relay_3, info->pins.gpio_relay_4);
}

bool board_supports_i2c(const board_info_t *info) {
    return info && info->caps.i2c_bus_count > 0;
}

bool board_supports_spi(const board_info_t *info) {
    return info && info->caps.spi_bus_count > 0;
}

bool board_supports_onewire(const board_info_t *info) {
    return info && info->pins.onewire_data >= 0;
}

bool board_supports_pwm(const board_info_t *info) {
    return info && info->caps.pwm_channels > 0;
}
