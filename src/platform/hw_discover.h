/**
 * @file hw_discover.h
 * @brief Hardware device discovery (I2C, 1-Wire, SPI)
 *
 * Scans system buses for connected devices and provides
 * device identification and enumeration.
 */

#ifndef HW_DISCOVER_H
#define HW_DISCOVER_H

#include "common.h"

/* Maximum devices per bus */
#define MAX_I2C_DEVICES 16
#define MAX_ONEWIRE_DEVICES 16

/* ============================================================================
 * I2C Device Types
 * ========================================================================== */

typedef enum {
    I2C_DEVICE_UNKNOWN = 0,
    I2C_DEVICE_ADS1115,        /* ADS1115 16-bit ADC */
    I2C_DEVICE_ADS1015,        /* ADS1015 12-bit ADC */
    I2C_DEVICE_MCP3421,        /* MCP3421 18-bit ADC */
    I2C_DEVICE_BME280,         /* BME280 Temp/Humidity/Pressure */
    I2C_DEVICE_BMP280,         /* BMP280 Temp/Pressure */
    I2C_DEVICE_SHT31,          /* SHT31 Temp/Humidity */
    I2C_DEVICE_HTU21D,         /* HTU21D Temp/Humidity */
    I2C_DEVICE_INA219,         /* INA219 Current/Power */
    I2C_DEVICE_PCA9685,        /* PCA9685 PWM Controller */
    I2C_DEVICE_PCF8574,        /* PCF8574 I/O Expander */
    I2C_DEVICE_MCP23017,       /* MCP23017 I/O Expander */
    I2C_DEVICE_AT24C,          /* AT24C EEPROM */
    I2C_DEVICE_DS3231,         /* DS3231 RTC */
    I2C_DEVICE_OLED_SSD1306,   /* SSD1306 OLED Display */
} i2c_device_type_t;

typedef struct {
    int bus;                   /* I2C bus number (0, 1, etc.) */
    uint8_t address;           /* 7-bit I2C address */
    i2c_device_type_t type;    /* Detected device type */
    char name[32];             /* Human-readable name */
    char description[64];      /* Suggested use */
    bool detected;             /* Device responded to scan */
} i2c_device_t;

/* ============================================================================
 * 1-Wire Device Types
 * ========================================================================== */

typedef enum {
    ONEWIRE_DEVICE_UNKNOWN = 0,
    ONEWIRE_DEVICE_DS18B20,    /* DS18B20 Temperature sensor */
    ONEWIRE_DEVICE_DS18S20,    /* DS18S20 Temperature sensor */
    ONEWIRE_DEVICE_DS1820,     /* DS1820 Temperature sensor */
    ONEWIRE_DEVICE_DS28EA00,   /* DS28EA00 Temperature sensor */
} onewire_device_type_t;

typedef struct {
    char id[20];               /* 64-bit ROM ID as string (e.g., "28-00000abcdef1") */
    onewire_device_type_t type;/* Detected device type */
    char name[32];             /* Human-readable name */
    char description[64];      /* Suggested use */
    float last_value;          /* Last read value (if applicable) */
} onewire_device_t;

/* ============================================================================
 * Discovery Results
 * ========================================================================== */

typedef struct {
    i2c_device_t i2c_devices[MAX_I2C_DEVICES];
    int i2c_count;

    onewire_device_t onewire_devices[MAX_ONEWIRE_DEVICES];
    int onewire_count;

    bool scan_complete;
    char error_message[128];
} hw_discovery_result_t;

/* ============================================================================
 * API Functions
 * ========================================================================== */

/**
 * Scan I2C bus for devices
 *
 * @param bus I2C bus number (0 or 1 typically)
 * @param result Output discovery results
 * @return RESULT_OK on success
 */
result_t hw_discover_i2c(int bus, hw_discovery_result_t *result);

/**
 * Scan all available I2C buses
 */
result_t hw_discover_i2c_all(hw_discovery_result_t *result);

/**
 * Scan 1-Wire bus for devices
 *
 * @param result Output discovery results
 * @return RESULT_OK on success
 */
result_t hw_discover_onewire(hw_discovery_result_t *result);

/**
 * Full hardware discovery (I2C + 1-Wire)
 */
result_t hw_discover_all(hw_discovery_result_t *result);

/**
 * Get device type name
 */
const char* i2c_device_type_name(i2c_device_type_t type);
const char* onewire_device_type_name(onewire_device_type_t type);

/**
 * Identify I2C device type from address
 */
i2c_device_type_t i2c_identify_device(int bus, uint8_t address);

#endif /* HW_DISCOVER_H */
