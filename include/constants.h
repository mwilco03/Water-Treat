/**
 * @file constants.h
 * @brief Centralized constants for the Water-Treat RTU
 *
 * This file is the SINGLE SOURCE OF TRUTH for magic numbers, default values,
 * and repeated strings throughout the codebase. If you need to change a
 * default value, change it HERE, not scattered across multiple files.
 *
 * Categories:
 * - Application identity
 * - File system paths
 * - Hardware defaults (GPIO, I2C, SPI)
 * - Network/HTTP constants
 * - Timeouts and intervals
 * - Buffer sizes
 * - PROFINET constants (see also profinet_identity.h)
 * - Driver identifiers
 * - Module/actuator/status type strings
 * - CRC and protocol constants
 */

#ifndef CONSTANTS_H
#define CONSTANTS_H

/* ============================================================================
 * Application Identity
 * ========================================================================== */

#define APP_NAME                    "water-treat"
#define APP_NAME_DISPLAY            "Water-Treat RTU"

/* ============================================================================
 * File System Paths (FHS Compliant)
 * ========================================================================== */

/** Configuration directory and files */
#define PATH_CONFIG_DIR             "/etc/water-treat"
#define PATH_CONFIG_FILE            "/etc/water-treat/water-treat.conf"
#define PATH_GSDML_DIR              "/etc/water-treat/gsd"

/** Variable data directory */
#define PATH_DATA_DIR               "/var/lib/water-treat"
#define PATH_DATABASE               "/var/lib/water-treat/data.db"
#define PATH_PROFINET_DATA          "/var/lib/water-treat/pnet"

/** Runtime directory (ephemeral, cleared on reboot) */
#define PATH_RUN_DIR                "/run/water-treat"

/** Log directory */
#define PATH_LOG_DIR                "/var/log/water-treat"

/** Backup directory */
#define PATH_BACKUP_DIR             "/var/backup/water-treat"

/* ============================================================================
 * Hardware Defaults
 * ========================================================================== */

/** Default GPIO chip for all GPIO operations */
#define DEFAULT_GPIO_CHIP           "gpiochip0"

/** Default SPI bus for ADC/DAC operations */
#define DEFAULT_SPI_BUS             0
#define DEFAULT_SPI_DEVICE          "/dev/spidev0.0"
#define DEFAULT_SPI_SPEED_HZ        2400000     /* WS2812 LED timing */

/** Default I2C bus */
#define DEFAULT_I2C_BUS             1
#define DEFAULT_I2C_DEVICE          "/dev/i2c-1"

/** I2C address scanning range (7-bit addresses) */
#define I2C_ADDR_MIN                0x03
#define I2C_ADDR_MAX                0x77
#define I2C_ADDR_RESERVED_MIN       0x30        /* Reserved range start */
#define I2C_ADDR_RESERVED_MAX       0x37        /* Reserved range end */
#define I2C_ADDR_10BIT_MIN          0x78        /* 10-bit addressing start */
#define I2C_ADDR_10BIT_MAX          0x7F        /* 10-bit addressing end */

/** Default LED brightness (0-255, 64 = 25%) */
#define DEFAULT_LED_BRIGHTNESS      64

/** Default PWM frequency for actuators */
#define DEFAULT_PWM_FREQUENCY_HZ    1000

/** Serial baud rates */
#define BAUD_RATE_DEFAULT           115200
#define BAUD_RATE_9600              9600
#define BAUD_RATE_19200             19200
#define BAUD_RATE_38400             38400
#define BAUD_RATE_57600             57600
#define BAUD_RATE_115200            115200
#define BAUD_RATE_230400            230400

/* ============================================================================
 * I2C Device Addresses (7-bit)
 * These are the DEFAULT addresses; actual address may be configured per-device
 * ========================================================================== */

/* Temperature/Humidity/Pressure sensors */
#define I2C_ADDR_BME280_PRIMARY     0x76    /* SDO to GND */
#define I2C_ADDR_BME280_SECONDARY   0x77    /* SDO to VCC */
#define I2C_ADDR_BMP280_PRIMARY     0x76
#define I2C_ADDR_BMP280_SECONDARY   0x77

/* ADC */
#define I2C_ADDR_ADS1115_GND        0x48    /* ADDR to GND */
#define I2C_ADDR_ADS1115_VDD        0x49    /* ADDR to VDD */
#define I2C_ADDR_ADS1115_SDA        0x4A    /* ADDR to SDA */
#define I2C_ADDR_ADS1115_SCL        0x4B    /* ADDR to SCL */

/* Color sensor */
#define I2C_ADDR_TCS34725           0x29

/* Load cell ADC */
#define I2C_ADDR_HX711              0x00    /* HX711 uses GPIO, not I2C - placeholder */

/* EEPROM */
#define I2C_ADDR_AT24C_BASE         0x50    /* A0=A1=A2=GND, range 0x50-0x57 */

/* GPIO expander */
#define I2C_ADDR_PCF8574_BASE       0x20    /* A0=A1=A2=GND, range 0x20-0x27 */
#define I2C_ADDR_PCF8574A_BASE      0x38    /* A0=A1=A2=GND, range 0x38-0x3F */
#define I2C_ADDR_MCP23017_BASE      0x20    /* A0=A1=A2=GND, range 0x20-0x27 */

/* ============================================================================
 * Network / HTTP Constants
 * ========================================================================== */

/** HTTP content types */
#define CONTENT_TYPE_JSON           "application/json"
#define CONTENT_TYPE_XML            "application/xml"
#define CONTENT_TYPE_HTML           "text/html"
#define CONTENT_TYPE_PLAIN          "text/plain"
#define CONTENT_TYPE_PROMETHEUS     "text/plain; version=0.0.4; charset=utf-8"

/** HTTP status codes */
#define HTTP_STATUS_OK              200
#define HTTP_STATUS_CREATED         201
#define HTTP_STATUS_NO_CONTENT      204
#define HTTP_STATUS_BAD_REQUEST     400
#define HTTP_STATUS_UNAUTHORIZED    401
#define HTTP_STATUS_FORBIDDEN       403
#define HTTP_STATUS_NOT_FOUND       404
#define HTTP_STATUS_CONFLICT        409
#define HTTP_STATUS_INTERNAL_ERROR  500
#define HTTP_STATUS_UNAVAILABLE     503

/** Port numbers */
#define DEFAULT_HTTP_PORT           9081    /* RTU HTTP API */
#define CONTROLLER_API_PORT         8000    /* SCADA controller REST API */
#define PROFINET_RPC_PORT           34964   /* PROFINET DCE/RPC (standard) */

/* ============================================================================
 * Timeouts and Intervals (all in milliseconds unless noted)
 * ========================================================================== */

/* Connection timeouts */
#define TIMEOUT_CONNECT_MS          5000    /* TCP/network connect timeout */
#define TIMEOUT_HTTP_MS             10000   /* HTTP request timeout */
#define TIMEOUT_I2C_MS              100     /* I2C transaction timeout */
#define TIMEOUT_SPI_MS              100     /* SPI transaction timeout */

/* PROFINET timeouts */
#define TIMEOUT_PROFINET_CONNECT_MS 5000    /* AR establishment timeout */
#define TIMEOUT_PROFINET_LIVENESS_MS 10000  /* Connection liveness check */
#define TIMEOUT_PROFINET_WATCHDOG_MS 30000  /* Default controller watchdog */

/* Sensor timeouts */
#define TIMEOUT_SENSOR_STALE_MS     5000    /* Sensor reading considered stale */
#define TIMEOUT_SENSOR_DEFAULT_MS   5000    /* Default sensor read timeout */
#define TIMEOUT_DHT22_MS            2000    /* DHT22 requires long wait */
#define TIMEOUT_DS18B20_MS          1000    /* 1-Wire conversion time */
#define TIMEOUT_BME280_MS           100     /* BME280 measurement */
#define TIMEOUT_ADS1115_MS          10      /* ADS1115 conversion */
#define TIMEOUT_ULTRASONIC_MS       60      /* JSN-SR04T echo timeout */

/* Actuator timeouts */
#define TIMEOUT_ACTUATOR_COMMAND_MS 5000    /* Command execution timeout */

/* Registration timeouts */
#define TIMEOUT_REGISTRATION_MS     60000   /* Controller registration timeout */
#define TIMEOUT_RETRY_MAX_MS        300000  /* Maximum retry delay (5 minutes) */

/* Retry intervals */
#define RETRY_INTERVAL_SHORT_MS     100     /* Quick retry */
#define RETRY_INTERVAL_MEDIUM_MS    1000    /* 1 second retry */
#define RETRY_INTERVAL_LONG_MS      5000    /* 5 second retry */
#define RETRY_COUNT_DEFAULT         60      /* Default retry count */

/* Periodic intervals */
#define INTERVAL_HEALTH_CHECK_MS    10000   /* Health check update */
#define INTERVAL_SENSOR_POLL_MS     1000    /* Default sensor poll rate */
#define INTERVAL_LOG_FLUSH_MS       60000   /* Log flush interval (1 minute) */
#define INTERVAL_WEB_POLL_MS        60000   /* Web poll sensor default */

/* Time limits (in seconds) */
#define LIMIT_LOG_INTERVAL_MIN_SEC  1       /* Minimum log interval */
#define LIMIT_LOG_INTERVAL_MAX_SEC  3600    /* Maximum log interval (1 hour) */
#define LIMIT_RETENTION_MIN_DAYS    1       /* Minimum retention */
#define LIMIT_RETENTION_MAX_DAYS    86400   /* Maximum retention (seconds, 1 day) */

/* ============================================================================
 * Buffer Sizes
 * ========================================================================== */

#define BUFFER_SIZE_TINY            32
#define BUFFER_SIZE_SMALL           64
#define BUFFER_SIZE_MEDIUM          128
#define BUFFER_SIZE_LARGE           256
#define BUFFER_SIZE_XLARGE          512
#define BUFFER_SIZE_HUGE            1024
#define BUFFER_SIZE_2K              2048
#define BUFFER_SIZE_4K              4096
#define BUFFER_SIZE_8K              8192
#define BUFFER_SIZE_16K             16384
#define BUFFER_SIZE_PATH            256     /* File path buffer */
#define BUFFER_SIZE_NAME            64      /* Name/identifier buffer */
#define BUFFER_SIZE_ERROR           256     /* Error message buffer */
#define BUFFER_SIZE_QUERY           1024    /* SQL query buffer */
#define BUFFER_SIZE_RESPONSE        4096    /* HTTP response buffer */

/* ============================================================================
 * PROFINET Slot Configuration
 * ========================================================================== */

/** Maximum number of PROFINET slots (DAP at 0, apps at 1-246) */
#define MAX_PROFINET_SLOTS          247
#define MAX_PROFINET_APP_SLOTS      246     /* Excludes DAP */
#define PROFINET_DAP_SLOT           0

/** PROFINET record indices (vendor-specific range 0xF840-0xF8FF) */
#define PROFINET_RECORD_USER_SYNC       0xF840  /* User sync from controller */
#define PROFINET_RECORD_DEVICE_CONFIG   0xF841  /* Device configuration */
#define PROFINET_RECORD_SENSOR_CONFIG   0xF842  /* Sensor configuration */
#define PROFINET_RECORD_ACTUATOR_CONFIG 0xF843  /* Actuator configuration */
#define PROFINET_RECORD_SLOT_MAP        0xF844  /* Slot map read */
#define PROFINET_RECORD_ENROLLMENT      0xF845  /* Enrollment/binding */

/** PROFINET I&M record indices (standard range) */
#define PROFINET_RECORD_IM0             0x8000
#define PROFINET_RECORD_IM1             0x8001
#define PROFINET_RECORD_IM2             0x8002
#define PROFINET_RECORD_IM3             0x8003
#define PROFINET_RECORD_IM4             0x8004

/** PROFINET parameter index range (standard) */
#define PROFINET_PARAM_INDEX_MAX        0x7FFF

/** Module identifier flags */
#define MODULE_IDENT_ACTUATOR_FLAG      0x100   /* Bit flag for actuator modules */

/* ============================================================================
 * CRC Constants (Protocol-specific)
 * ========================================================================== */

#define CRC16_INIT                  0xFFFF      /* CRC-16 initialization */
#define CRC16_CCITT_POLY            0x1021      /* CRC-16-CCITT polynomial */

/* ============================================================================
 * Driver Type Identifiers
 * Use these instead of hardcoded strings for driver type checks
 * ========================================================================== */

#define DRIVER_NAME_DS18B20         "DS18B20"
#define DRIVER_NAME_DHT22           "DHT22"
#define DRIVER_NAME_DHT11           "DHT11"
#define DRIVER_NAME_BME280          "BME280"
#define DRIVER_NAME_BMP280          "BMP280"
#define DRIVER_NAME_ADS1115         "ADS1115"
#define DRIVER_NAME_ADS1015         "ADS1015"
#define DRIVER_NAME_MCP3008         "MCP3008"
#define DRIVER_NAME_HX711           "HX711"
#define DRIVER_NAME_TCS34725        "TCS34725"
#define DRIVER_NAME_JSN_SR04T       "JSN-SR04T"
#define DRIVER_NAME_AT24C           "AT24C"
#define DRIVER_NAME_PCF8574         "PCF8574"
#define DRIVER_NAME_MCP23017        "MCP23017"
#define DRIVER_NAME_FLOAT_SWITCH    "FloatSwitch"
#define DRIVER_NAME_WEB_POLL        "WebPoll"

/* ============================================================================
 * Module Type Identifiers
 * Use these for module_type field comparisons
 * ========================================================================== */

#define MODULE_TYPE_PHYSICAL        "physical"
#define MODULE_TYPE_ADC             "adc"
#define MODULE_TYPE_WEB_POLL        "web_poll"
#define MODULE_TYPE_SIMULATED       "simulated"

/* ============================================================================
 * Actuator Type Identifiers (String Constants)
 * Use these for actuator type field comparisons in database/serialization.
 * NOTE: These use _STR suffix to avoid conflict with actuator_type_t enum values.
 * ========================================================================== */

#define ACTUATOR_TYPE_RELAY_STR     "relay"
#define ACTUATOR_TYPE_PWM_STR       "pwm"
#define ACTUATOR_TYPE_LATCHING_STR  "latching"
#define ACTUATOR_TYPE_MOMENTARY_STR "momentary"

/* ============================================================================
 * Status/State Identifiers
 * Use these for status field comparisons
 * ========================================================================== */

#define STATUS_OK                   "ok"
#define STATUS_ERROR                "error"
#define STATUS_UNKNOWN              "unknown"
#define STATUS_DISABLED             "disabled"
#define STATUS_ACTIVE               "active"
#define STATUS_ACKNOWLEDGED         "acknowledged"
#define STATUS_CLEARED              "cleared"

/* ============================================================================
 * Interface Type Identifiers
 * ========================================================================== */

#define INTERFACE_I2C               "i2c"
#define INTERFACE_SPI               "spi"
#define INTERFACE_1WIRE             "1wire"
#define INTERFACE_GPIO              "gpio"
#define INTERFACE_UART              "uart"

/* ============================================================================
 * Database Defaults
 * ========================================================================== */

#define DEFAULT_DB_BUSY_TIMEOUT_MS  5000
#define DEFAULT_DB_PATH             PATH_DATABASE

/* ============================================================================
 * Logging Defaults
 * ========================================================================== */

#define DEFAULT_LOG_INTERVAL_SEC    60
#define DEFAULT_LOG_RETENTION_DAYS  30

/* ============================================================================
 * Station Name Defaults
 * ========================================================================== */

/** Fallback station name when MAC detection fails */
#define FALLBACK_STATION_NAME       "rtu-0000"

/** Station name prefix (followed by last 4 hex chars of MAC) */
#define STATION_NAME_PREFIX         "rtu-"

/* ============================================================================
 * Authentication
 * ========================================================================== */

/** Default admin credentials (MUST always work per CLAUDE.md) */
#define DEFAULT_ADMIN_USERNAME      "admin"
#define DEFAULT_ADMIN_PASSWORD      "H2OhYeah!"

/** Password hashing salt */
#define AUTH_HASH_SALT              "NaCl4Life"

/** Maximum synced users from controller */
#define MAX_SYNCED_USERS            16

#endif /* CONSTANTS_H */
