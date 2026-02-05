/**
 * @file constants.h
 * @brief Centralized constants for the Water-Treat RTU
 *
 * This file is the SINGLE SOURCE OF TRUTH for magic numbers, default values,
 * and repeated strings throughout the codebase. If you need to change a
 * default value, change it HERE, not scattered across multiple files.
 *
 * Categories:
 * - Hardware defaults (GPIO, I2C, SPI)
 * - Network/HTTP constants
 * - Timeouts and intervals
 * - Buffer sizes
 * - PROFINET constants (see also profinet_identity.h)
 * - Driver identifiers
 */

#ifndef CONSTANTS_H
#define CONSTANTS_H

/* ============================================================================
 * Hardware Defaults
 * ========================================================================== */

/** Default GPIO chip for all GPIO operations */
#define DEFAULT_GPIO_CHIP           "gpiochip0"

/** Default SPI bus for ADC/DAC operations */
#define DEFAULT_SPI_BUS             0
#define DEFAULT_SPI_DEVICE          "/dev/spidev0.0"

/** Default I2C bus */
#define DEFAULT_I2C_BUS             1
#define DEFAULT_I2C_DEVICE          "/dev/i2c-1"

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
#define CONTENT_TYPE_PROMETHEUS     "text/plain; version=0.0.4"

/** HTTP status codes (common ones) */
#define HTTP_STATUS_OK              200
#define HTTP_STATUS_CREATED         201
#define HTTP_STATUS_NO_CONTENT      204
#define HTTP_STATUS_BAD_REQUEST     400
#define HTTP_STATUS_UNAUTHORIZED    401
#define HTTP_STATUS_FORBIDDEN       403
#define HTTP_STATUS_NOT_FOUND       404
#define HTTP_STATUS_INTERNAL_ERROR  500
#define HTTP_STATUS_UNAVAILABLE     503

/** Default HTTP port for RTU API */
#define DEFAULT_HTTP_PORT           9081

/** PROFINET DCE/RPC port (standard, do not change) */
#define PROFINET_RPC_PORT           34964

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
#define TIMEOUT_PROFINET_WATCHDOG_DEFAULT_MS 30000  /* Default controller watchdog */

/* Sensor read timeouts */
#define TIMEOUT_DHT22_MS            2000    /* DHT22 requires long wait */
#define TIMEOUT_DS18B20_MS          1000    /* 1-Wire conversion time */
#define TIMEOUT_BME280_MS           100     /* BME280 measurement */
#define TIMEOUT_ADS1115_MS          10      /* ADS1115 conversion */
#define TIMEOUT_ULTRASONIC_MS       60      /* JSN-SR04T echo timeout */

/* Retry intervals */
#define RETRY_INTERVAL_SHORT_MS     100     /* Quick retry */
#define RETRY_INTERVAL_MEDIUM_MS    1000    /* 1 second retry */
#define RETRY_INTERVAL_LONG_MS      5000    /* 5 second retry */

/* Periodic intervals */
#define INTERVAL_HEALTH_CHECK_MS    10000   /* Health check update */
#define INTERVAL_SENSOR_POLL_MS     1000    /* Default sensor poll rate */
#define INTERVAL_LOG_FLUSH_MS       60000   /* Log flush interval */

/* ============================================================================
 * Buffer Sizes
 * ========================================================================== */

#define BUFFER_SIZE_TINY            32
#define BUFFER_SIZE_SMALL           64
#define BUFFER_SIZE_MEDIUM          128
#define BUFFER_SIZE_LARGE           256
#define BUFFER_SIZE_XLARGE          512
#define BUFFER_SIZE_HUGE            1024
#define BUFFER_SIZE_PATH            256     /* File path buffer */
#define BUFFER_SIZE_NAME            64      /* Name/identifier buffer */
#define BUFFER_SIZE_ERROR           256     /* Error message buffer */

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

/** PROFINET I&M record index (standard) */
#define PROFINET_RECORD_IM0             0x8000

/* ============================================================================
 * Driver Type Identifiers
 * Use these instead of hardcoded strings for driver type checks
 * ========================================================================== */

#define DRIVER_NAME_DS18B20         "DS18B20"
#define DRIVER_NAME_DHT22           "DHT22"
#define DRIVER_NAME_BME280          "BME280"
#define DRIVER_NAME_BMP280          "BMP280"
#define DRIVER_NAME_ADS1115         "ADS1115"
#define DRIVER_NAME_MCP3008         "MCP3008"
#define DRIVER_NAME_HX711           "HX711"
#define DRIVER_NAME_TCS34725        "TCS34725"
#define DRIVER_NAME_JSN_SR04T       "JSN-SR04T"
#define DRIVER_NAME_AT24C           "AT24C"
#define DRIVER_NAME_PCF8574         "PCF8574"
#define DRIVER_NAME_MCP23017        "MCP23017"
#define DRIVER_NAME_PHYSICAL        "physical"      /* Direct GPIO */
#define DRIVER_NAME_SIMULATED       "simulated"     /* Simulated/test */

/* ============================================================================
 * Database Defaults
 * ========================================================================== */

#define DEFAULT_DB_BUSY_TIMEOUT_MS  5000
#define DEFAULT_DB_PATH             "/var/lib/water-treat/data.db"

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
