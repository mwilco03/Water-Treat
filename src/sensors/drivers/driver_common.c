/**
 * @file driver_common.c
 * @brief Common sensor driver infrastructure implementation
 */

#include "driver_common.h"
#include <string.h>
#include <strings.h>

/* Forward declarations of driver ops tables (fully converted drivers) */
extern const driver_ops_t driver_ds18b20_ops;
extern const driver_ops_t driver_dht22_ops;
extern const driver_ops_t driver_bme280_ops;
extern const driver_ops_t driver_hx711_ops;
extern const driver_ops_t driver_ads1115_ops;
extern const driver_ops_t driver_mcp3008_ops;

/*
 * Stub ops for drivers not yet converted to driver_common pattern.
 * These are used by sensor_instance.c directly and don't need the
 * standard ops interface, but we register them for name lookup.
 */
static const driver_ops_t driver_jsn_sr04t_ops = {
    .read = NULL,
    .close = NULL,
    .set_calibration = NULL,
    .name = "JSN-SR04T"
};

static const driver_ops_t driver_tcs34725_ops = {
    .read = NULL,
    .close = NULL,
    .set_calibration = NULL,
    .name = "TCS34725"
};

static const driver_ops_t driver_float_switch_ops = {
    .read = NULL,
    .close = NULL,
    .set_calibration = NULL,
    .name = "FloatSwitch"
};

static const driver_ops_t driver_web_poll_ops = {
    .read = NULL,
    .close = NULL,
    .set_calibration = NULL,
    .name = "WebPoll"
};

/**
 * Driver registry - maps type to ops table
 *
 * Add new drivers here when implementing them.
 */
static const driver_registry_entry_t driver_registry[] = {
    { DRIVER_TYPE_DS18B20,     "DS18B20",     &driver_ds18b20_ops },
    { DRIVER_TYPE_DHT22,       "DHT22",       &driver_dht22_ops },
    { DRIVER_TYPE_BME280,      "BME280",      &driver_bme280_ops },
    { DRIVER_TYPE_HX711,       "HX711",       &driver_hx711_ops },
    { DRIVER_TYPE_JSN_SR04T,   "JSN-SR04T",   &driver_jsn_sr04t_ops },
    { DRIVER_TYPE_TCS34725,    "TCS34725",    &driver_tcs34725_ops },
    { DRIVER_TYPE_FLOAT_SWITCH,"FloatSwitch", &driver_float_switch_ops },
    { DRIVER_TYPE_ADS1115,     "ADS1115",     &driver_ads1115_ops },
    { DRIVER_TYPE_MCP3008,     "MCP3008",     &driver_mcp3008_ops },
    { DRIVER_TYPE_WEB_POLL,    "WebPoll",     &driver_web_poll_ops },
    { DRIVER_TYPE_NONE,        NULL,          NULL }  /* Sentinel */
};

const driver_ops_t *driver_get_ops_by_type(driver_type_t type) {
    for (int i = 0; driver_registry[i].name != NULL; i++) {
        if (driver_registry[i].type == type) {
            return driver_registry[i].ops;
        }
    }
    return NULL;
}

const driver_ops_t *driver_get_ops_by_name(const char *name) {
    if (!name) return NULL;

    for (int i = 0; driver_registry[i].name != NULL; i++) {
        if (strcasecmp(driver_registry[i].name, name) == 0) {
            return driver_registry[i].ops;
        }
    }

    /* Handle aliases */
    if (strcasecmp(name, "DHT11") == 0) {
        return driver_get_ops_by_type(DRIVER_TYPE_DHT22);
    }
    if (strcasecmp(name, "AM2302") == 0) {
        return driver_get_ops_by_type(DRIVER_TYPE_DHT22);
    }
    if (strcasecmp(name, "BMP280") == 0) {
        return driver_get_ops_by_type(DRIVER_TYPE_BME280);
    }
    if (strcasecmp(name, "ADS1015") == 0) {
        return driver_get_ops_by_type(DRIVER_TYPE_ADS1115);
    }

    return NULL;
}

driver_type_t driver_get_type_by_name(const char *name) {
    if (!name) return DRIVER_TYPE_NONE;

    for (int i = 0; driver_registry[i].name != NULL; i++) {
        if (strcasecmp(driver_registry[i].name, name) == 0) {
            return driver_registry[i].type;
        }
    }

    /* Handle aliases */
    if (strcasecmp(name, "DHT11") == 0 || strcasecmp(name, "AM2302") == 0) {
        return DRIVER_TYPE_DHT22;
    }
    if (strcasecmp(name, "BMP280") == 0) {
        return DRIVER_TYPE_BME280;
    }
    if (strcasecmp(name, "ADS1015") == 0) {
        return DRIVER_TYPE_ADS1115;
    }

    return DRIVER_TYPE_NONE;
}
