/**
 * @file hw_discover.c
 * @brief Hardware device discovery implementation
 *
 * Scans I2C buses and 1-Wire interfaces for connected devices.
 */

#include "hw_discover.h"
#include "utils/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <glob.h>
#include <sys/ioctl.h>
#include <errno.h>

#ifdef __linux__
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#endif

/* ============================================================================
 * I2C Address to Device Type Mapping
 * ========================================================================== */

typedef struct {
    uint8_t address;
    i2c_device_type_t type;
    const char *name;
    const char *description;
} i2c_device_info_t;

/* Known I2C device addresses */
static const i2c_device_info_t known_i2c_devices[] = {
    /* ADCs */
    {0x48, I2C_DEVICE_ADS1115, "ADS1115/ADS1015", "4-ch ADC (addr 0x48)"},
    {0x49, I2C_DEVICE_ADS1115, "ADS1115/ADS1015", "4-ch ADC (addr 0x49)"},
    {0x4A, I2C_DEVICE_ADS1115, "ADS1115/ADS1015", "4-ch ADC (addr 0x4A)"},
    {0x4B, I2C_DEVICE_ADS1115, "ADS1115/ADS1015", "4-ch ADC (addr 0x4B)"},
    {0x68, I2C_DEVICE_MCP3421, "MCP3421", "18-bit ADC"},
    {0x69, I2C_DEVICE_MCP3421, "MCP3421", "18-bit ADC (addr 0x69)"},

    /* Environmental sensors */
    {0x76, I2C_DEVICE_BME280, "BME280/BMP280", "Temp/Humidity/Pressure"},
    {0x77, I2C_DEVICE_BME280, "BME280/BMP280", "Temp/Humidity/Pressure (alt)"},
    {0x44, I2C_DEVICE_SHT31, "SHT31", "Temp/Humidity sensor"},
    {0x45, I2C_DEVICE_SHT31, "SHT31", "Temp/Humidity (alt addr)"},
    {0x40, I2C_DEVICE_HTU21D, "HTU21D", "Temp/Humidity sensor"},

    /* Power monitoring */
    {0x41, I2C_DEVICE_INA219, "INA219", "Current/Power monitor"},
    {0x42, I2C_DEVICE_INA219, "INA219", "Current/Power (addr 0x42)"},
    {0x43, I2C_DEVICE_INA219, "INA219", "Current/Power (addr 0x43)"},

    /* PWM/GPIO expanders */
    {0x40, I2C_DEVICE_PCA9685, "PCA9685", "16-ch PWM controller"},
    {0x41, I2C_DEVICE_PCA9685, "PCA9685", "PWM controller (alt)"},
    {0x20, I2C_DEVICE_PCF8574, "PCF8574", "8-bit I/O expander"},
    {0x21, I2C_DEVICE_PCF8574, "PCF8574", "I/O expander (addr 0x21)"},
    {0x22, I2C_DEVICE_PCF8574, "PCF8574", "I/O expander (addr 0x22)"},
    {0x23, I2C_DEVICE_PCF8574, "PCF8574", "I/O expander (addr 0x23)"},
    {0x24, I2C_DEVICE_MCP23017, "MCP23017", "16-bit I/O expander"},
    {0x25, I2C_DEVICE_MCP23017, "MCP23017", "I/O expander (addr 0x25)"},
    {0x26, I2C_DEVICE_MCP23017, "MCP23017", "I/O expander (addr 0x26)"},
    {0x27, I2C_DEVICE_MCP23017, "MCP23017", "I/O expander (addr 0x27)"},

    /* EEPROM */
    {0x50, I2C_DEVICE_AT24C, "AT24C EEPROM", "I2C EEPROM"},
    {0x51, I2C_DEVICE_AT24C, "AT24C EEPROM", "EEPROM (addr 0x51)"},
    {0x52, I2C_DEVICE_AT24C, "AT24C EEPROM", "EEPROM (addr 0x52)"},
    {0x53, I2C_DEVICE_AT24C, "AT24C EEPROM", "EEPROM (addr 0x53)"},
    {0x54, I2C_DEVICE_AT24C, "AT24C EEPROM", "EEPROM (addr 0x54)"},
    {0x55, I2C_DEVICE_AT24C, "AT24C EEPROM", "EEPROM (addr 0x55)"},
    {0x56, I2C_DEVICE_AT24C, "AT24C EEPROM", "EEPROM (addr 0x56)"},
    {0x57, I2C_DEVICE_AT24C, "AT24C EEPROM", "EEPROM (addr 0x57)"},

    /* RTC */
    {0x68, I2C_DEVICE_DS3231, "DS3231", "Real-time clock"},

    /* Display */
    {0x3C, I2C_DEVICE_OLED_SSD1306, "SSD1306", "OLED Display 128x64"},
    {0x3D, I2C_DEVICE_OLED_SSD1306, "SSD1306", "OLED Display (alt)"},

    {0, I2C_DEVICE_UNKNOWN, NULL, NULL}  /* Terminator */
};

/* ============================================================================
 * 1-Wire Device Family Codes
 * ========================================================================== */

typedef struct {
    uint8_t family_code;
    onewire_device_type_t type;
    const char *name;
    const char *description;
} onewire_family_info_t;

static const onewire_family_info_t onewire_families[] = {
    {0x28, ONEWIRE_DEVICE_DS18B20, "DS18B20", "Digital temperature sensor"},
    {0x10, ONEWIRE_DEVICE_DS18S20, "DS18S20", "Digital temperature sensor"},
    {0x22, ONEWIRE_DEVICE_DS1820,  "DS1820",  "Digital temperature sensor"},
    {0x42, ONEWIRE_DEVICE_DS28EA00, "DS28EA00", "Temperature sensor w/PIO"},
    {0, ONEWIRE_DEVICE_UNKNOWN, NULL, NULL}  /* Terminator */
};

/* ============================================================================
 * I2C Device Probing
 * ========================================================================== */

#ifdef __linux__

/**
 * Check if an I2C device responds at given address
 */
static bool i2c_probe_address(int fd, uint8_t address)
{
    /* Use quick write for probing - most devices will ACK */
    struct i2c_smbus_ioctl_data args;
    union i2c_smbus_data data;

    args.read_write = I2C_SMBUS_WRITE;
    args.command = 0;
    args.size = I2C_SMBUS_QUICK;
    args.data = &data;

    if (ioctl(fd, I2C_SLAVE, address) < 0) {
        return false;
    }

    /* Try quick write probe */
    if (ioctl(fd, I2C_SMBUS, &args) >= 0) {
        return true;
    }

    /* Some devices don't respond to quick write, try read byte */
    args.read_write = I2C_SMBUS_READ;
    args.size = I2C_SMBUS_BYTE;
    return (ioctl(fd, I2C_SMBUS, &args) >= 0);
}

/**
 * Look up device info by address
 */
static const i2c_device_info_t* i2c_lookup_device(uint8_t address)
{
    for (int i = 0; known_i2c_devices[i].name != NULL; i++) {
        if (known_i2c_devices[i].address == address) {
            return &known_i2c_devices[i];
        }
    }
    return NULL;
}

result_t hw_discover_i2c(int bus, hw_discovery_result_t *result)
{
    if (!result) {
        return RESULT_INVALID_PARAM;
    }

    char bus_path[32];
    snprintf(bus_path, sizeof(bus_path), "/dev/i2c-%d", bus);

    int fd = open(bus_path, O_RDWR);
    if (fd < 0) {
        snprintf(result->error_message, sizeof(result->error_message),
                 "Cannot open %s: %s", bus_path, strerror(errno));
        LOG_WARNING("I2C discovery: %s", result->error_message);
        return RESULT_IO_ERROR;
    }

    LOG_INFO("Scanning I2C bus %d...", bus);

    /* Scan address range 0x03 to 0x77 (standard 7-bit addresses) */
    for (uint8_t addr = 0x03; addr <= 0x77 && result->i2c_count < MAX_I2C_DEVICES; addr++) {
        /* Skip reserved addresses */
        if (addr >= 0x30 && addr <= 0x37) continue;  /* Reserved */
        if (addr >= 0x78 && addr <= 0x7F) continue;  /* 10-bit addressing */

        if (i2c_probe_address(fd, addr)) {
            i2c_device_t *dev = &result->i2c_devices[result->i2c_count];
            const i2c_device_info_t *info = i2c_lookup_device(addr);

            dev->bus = bus;
            dev->address = addr;
            dev->detected = true;

            if (info) {
                dev->type = info->type;
                strncpy(dev->name, info->name, sizeof(dev->name) - 1);
                strncpy(dev->description, info->description, sizeof(dev->description) - 1);
            } else {
                dev->type = I2C_DEVICE_UNKNOWN;
                snprintf(dev->name, sizeof(dev->name), "Unknown (0x%02X)", addr);
                snprintf(dev->description, sizeof(dev->description),
                         "Unknown device at address 0x%02X", addr);
            }

            LOG_INFO("  Found: 0x%02X - %s", addr, dev->name);
            result->i2c_count++;
        }
    }

    close(fd);
    LOG_INFO("I2C bus %d scan complete: %d device(s) found", bus, result->i2c_count);
    return RESULT_OK;
}

result_t hw_discover_i2c_all(hw_discovery_result_t *result)
{
    if (!result) {
        return RESULT_INVALID_PARAM;
    }

    /* Use glob to find all I2C bus devices */
    glob_t globbuf;
    if (glob("/dev/i2c-*", GLOB_NOSORT, NULL, &globbuf) == 0) {
        for (size_t i = 0; i < globbuf.gl_pathc; i++) {
            int bus;
            if (sscanf(globbuf.gl_pathv[i], "/dev/i2c-%d", &bus) == 1) {
                hw_discover_i2c(bus, result);
            }
        }
        globfree(&globbuf);
    }

    result->scan_complete = true;
    return RESULT_OK;
}

#else /* Non-Linux stub */

result_t hw_discover_i2c(int bus, hw_discovery_result_t *result)
{
    (void)bus;
    if (!result) return RESULT_INVALID_PARAM;
    snprintf(result->error_message, sizeof(result->error_message),
             "I2C discovery not supported on this platform");
    return RESULT_NOT_SUPPORTED;
}

result_t hw_discover_i2c_all(hw_discovery_result_t *result)
{
    if (!result) return RESULT_INVALID_PARAM;
    snprintf(result->error_message, sizeof(result->error_message),
             "I2C discovery not supported on this platform");
    return RESULT_NOT_SUPPORTED;
}

#endif /* __linux__ */

/* ============================================================================
 * 1-Wire Device Discovery
 * ========================================================================== */

/**
 * Parse 1-Wire family code from device ID
 */
static uint8_t onewire_parse_family(const char *device_id)
{
    unsigned int family;
    if (sscanf(device_id, "%02x-", &family) == 1) {
        return (uint8_t)family;
    }
    return 0;
}

/**
 * Look up 1-Wire device info by family code
 */
static const onewire_family_info_t* onewire_lookup_family(uint8_t family)
{
    for (int i = 0; onewire_families[i].name != NULL; i++) {
        if (onewire_families[i].family_code == family) {
            return &onewire_families[i];
        }
    }
    return NULL;
}

/**
 * Try to read current temperature from DS18B20
 */
static float onewire_read_temperature(const char *device_id)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/bus/w1/devices/%s/w1_slave", device_id);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -999.0f;  /* Invalid reading */
    }

    char line[128];
    float temp = -999.0f;

    /* Read two lines: CRC check and temperature */
    if (fgets(line, sizeof(line), fp)) {
        /* First line ends with "YES" if CRC valid */
        if (strstr(line, "YES")) {
            if (fgets(line, sizeof(line), fp)) {
                /* Second line contains "t=XXXXX" (temp in milli-degrees) */
                char *t_pos = strstr(line, "t=");
                if (t_pos) {
                    int raw_temp;
                    if (sscanf(t_pos + 2, "%d", &raw_temp) == 1) {
                        temp = raw_temp / 1000.0f;
                    }
                }
            }
        }
    }

    fclose(fp);
    return temp;
}

result_t hw_discover_onewire(hw_discovery_result_t *result)
{
    if (!result) {
        return RESULT_INVALID_PARAM;
    }

    const char *w1_path = "/sys/bus/w1/devices";
    DIR *dir = opendir(w1_path);
    if (!dir) {
        snprintf(result->error_message, sizeof(result->error_message),
                 "Cannot open %s: %s (1-Wire may not be enabled)",
                 w1_path, strerror(errno));
        LOG_WARNING("1-Wire discovery: %s", result->error_message);
        return RESULT_IO_ERROR;
    }

    LOG_INFO("Scanning 1-Wire bus...");

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && result->onewire_count < MAX_ONEWIRE_DEVICES) {
        /* Skip . and .. and w1_bus_master entries */
        if (entry->d_name[0] == '.') continue;
        if (strncmp(entry->d_name, "w1_bus_master", 13) == 0) continue;

        uint8_t family = onewire_parse_family(entry->d_name);
        if (family == 0) continue;  /* Not a valid device ID */

        onewire_device_t *dev = &result->onewire_devices[result->onewire_count];
        const onewire_family_info_t *info = onewire_lookup_family(family);

        strncpy(dev->id, entry->d_name, sizeof(dev->id) - 1);
        dev->id[sizeof(dev->id) - 1] = '\0';

        if (info) {
            dev->type = info->type;
            strncpy(dev->name, info->name, sizeof(dev->name) - 1);
            strncpy(dev->description, info->description, sizeof(dev->description) - 1);
        } else {
            dev->type = ONEWIRE_DEVICE_UNKNOWN;
            snprintf(dev->name, sizeof(dev->name), "Unknown (0x%02X)", family);
            snprintf(dev->description, sizeof(dev->description),
                     "Unknown 1-Wire device family 0x%02X", family);
        }

        /* Try to read temperature if it's a temp sensor */
        if (dev->type == ONEWIRE_DEVICE_DS18B20 ||
            dev->type == ONEWIRE_DEVICE_DS18S20 ||
            dev->type == ONEWIRE_DEVICE_DS1820) {
            dev->last_value = onewire_read_temperature(dev->id);
        } else {
            dev->last_value = -999.0f;
        }

        LOG_INFO("  Found: %s - %s", dev->id, dev->name);
        if (dev->last_value > -273.15f) {
            LOG_INFO("    Current reading: %.2f C", dev->last_value);
        }

        result->onewire_count++;
    }

    closedir(dir);
    LOG_INFO("1-Wire scan complete: %d device(s) found", result->onewire_count);
    return RESULT_OK;
}

result_t hw_discover_all(hw_discovery_result_t *result)
{
    if (!result) {
        return RESULT_INVALID_PARAM;
    }

    memset(result, 0, sizeof(hw_discovery_result_t));

    hw_discover_i2c_all(result);
    hw_discover_onewire(result);

    result->scan_complete = true;
    return RESULT_OK;
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

const char* i2c_device_type_name(i2c_device_type_t type)
{
    switch (type) {
        case I2C_DEVICE_UNKNOWN:     return "Unknown";
        case I2C_DEVICE_ADS1115:     return "ADS1115";
        case I2C_DEVICE_ADS1015:     return "ADS1015";
        case I2C_DEVICE_MCP3421:     return "MCP3421";
        case I2C_DEVICE_BME280:      return "BME280";
        case I2C_DEVICE_BMP280:      return "BMP280";
        case I2C_DEVICE_SHT31:       return "SHT31";
        case I2C_DEVICE_HTU21D:      return "HTU21D";
        case I2C_DEVICE_INA219:      return "INA219";
        case I2C_DEVICE_PCA9685:     return "PCA9685";
        case I2C_DEVICE_PCF8574:     return "PCF8574";
        case I2C_DEVICE_MCP23017:    return "MCP23017";
        case I2C_DEVICE_AT24C:       return "AT24C";
        case I2C_DEVICE_DS3231:      return "DS3231";
        case I2C_DEVICE_OLED_SSD1306: return "SSD1306";
        default:                     return "Unknown";
    }
}

const char* onewire_device_type_name(onewire_device_type_t type)
{
    switch (type) {
        case ONEWIRE_DEVICE_UNKNOWN:  return "Unknown";
        case ONEWIRE_DEVICE_DS18B20:  return "DS18B20";
        case ONEWIRE_DEVICE_DS18S20:  return "DS18S20";
        case ONEWIRE_DEVICE_DS1820:   return "DS1820";
        case ONEWIRE_DEVICE_DS28EA00: return "DS28EA00";
        default:                      return "Unknown";
    }
}

i2c_device_type_t i2c_identify_device(int bus, uint8_t address)
{
    const i2c_device_info_t *info = i2c_lookup_device(address);
    (void)bus;  /* Could add bus-specific identification in future */
    return info ? info->type : I2C_DEVICE_UNKNOWN;
}
