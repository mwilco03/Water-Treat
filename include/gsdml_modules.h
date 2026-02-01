/**
 * @file gsdml_modules.h
 * @brief GSDML module identifier mappings for PROFINET
 *
 * These values MUST match GSDML-V2.4-WaterTreat-RTU-20241222.xml
 * Controller and RTU must use identical module identifiers.
 *
 * Module ID pattern: 0x0000XXYY
 *   XX = module type (01=sensor, 10=actuator)
 *   YY = specific type within category
 *
 * Submodule ID = Module ID + 1 (for data submodule)
 */

#ifndef GSDML_MODULES_H
#define GSDML_MODULES_H

#include <stdint.h>
#include <string.h>
#include "profinet_identity.h"
#include "sensors/sensor_api.h"
#include "db/db_actuators.h"

/* ============================================================================
 * DAP (Device Access Point) - Slot 0
 * Derived from profinet_identity.h (single source of truth)
 * ========================================================================== */

#define GSDML_MOD_DAP              PN_MOD_DAP_IDENT
#define GSDML_SUBMOD_DAP           PN_SUBMOD_DAP_IDENT
#define GSDML_SUBMOD_DAP_INTERFACE PN_SUBMOD_DAP_IF_IDENT
#define GSDML_SUBMOD_DAP_PORT      PN_SUBMOD_DAP_PORT_IDENT

/* ============================================================================
 * Sensor Input Modules (Slots 1-8)
 * Per GSDML-V2.4-WaterTreat-RTU-20241222.xml
 * ========================================================================== */

#define GSDML_MOD_SENSOR_PH        0x00000010
#define GSDML_MOD_SENSOR_TDS       0x00000020
#define GSDML_MOD_SENSOR_TURBIDITY 0x00000030
#define GSDML_MOD_SENSOR_TEMP      0x00000040
#define GSDML_MOD_SENSOR_FLOW      0x00000050
#define GSDML_MOD_SENSOR_LEVEL     0x00000060
#define GSDML_MOD_SENSOR_GENERIC   0x00000070

/* Submodule = Module + 1 */
#define GSDML_SUBMOD_SENSOR_PH        0x00000011
#define GSDML_SUBMOD_SENSOR_TDS       0x00000021
#define GSDML_SUBMOD_SENSOR_TURBIDITY 0x00000031
#define GSDML_SUBMOD_SENSOR_TEMP      0x00000041
#define GSDML_SUBMOD_SENSOR_FLOW      0x00000051
#define GSDML_SUBMOD_SENSOR_LEVEL     0x00000061
#define GSDML_SUBMOD_SENSOR_GENERIC   0x00000071

/* ============================================================================
 * Actuator Output Modules (Slots 9-15)
 * Per GSDML-V2.4-WaterTreat-RTU-20241222.xml
 * ========================================================================== */

#define GSDML_MOD_ACTUATOR_PUMP    0x00000100
#define GSDML_MOD_ACTUATOR_VALVE   0x00000110
#define GSDML_MOD_ACTUATOR_GENERIC 0x00000120

/* Submodule = Module + 1 */
#define GSDML_SUBMOD_ACTUATOR_PUMP    0x00000101
#define GSDML_SUBMOD_ACTUATOR_VALVE   0x00000111
#define GSDML_SUBMOD_ACTUATOR_GENERIC 0x00000121

/* ============================================================================
 * I/O Data Sizes (per controller protocol agreement)
 * ========================================================================== */

/** Sensor input: 4-byte IEEE754 float (BE) + 1-byte quality = 5 bytes */
#define GSDML_SENSOR_INPUT_SIZE    5

/** Actuator output: 1-byte cmd + 1-byte duty + 2-byte reserved = 4 bytes */
#define GSDML_ACTUATOR_OUTPUT_SIZE 4

/* ============================================================================
 * Mapping Functions
 * ========================================================================== */

/**
 * @brief Get GSDML module identifier for a sensor channel type
 * @param channel Sensor channel type (pH, TDS, temperature, etc.)
 * @return GSDML module identifier
 */
static inline uint32_t gsdml_sensor_module_ident(sensor_channel_t channel) {
    switch (channel) {
        case SENSOR_CHAN_PH:
            return GSDML_MOD_SENSOR_PH;
        case SENSOR_CHAN_TDS:
        case SENSOR_CHAN_CONDUCTIVITY:
            return GSDML_MOD_SENSOR_TDS;
        case SENSOR_CHAN_TURBIDITY:
            return GSDML_MOD_SENSOR_TURBIDITY;
        case SENSOR_CHAN_TEMPERATURE:
        case SENSOR_CHAN_HUMIDITY:
            return GSDML_MOD_SENSOR_TEMP;
        case SENSOR_CHAN_PRESSURE:
        case SENSOR_CHAN_DISTANCE:
            return GSDML_MOD_SENSOR_FLOW;  /* Pressure/flow sensors */
        case SENSOR_CHAN_LEVEL:
        case SENSOR_CHAN_WEIGHT:
            return GSDML_MOD_SENSOR_LEVEL;
        default:
            return GSDML_MOD_SENSOR_GENERIC;
    }
}

/**
 * @brief Get GSDML submodule identifier for a sensor channel type
 * @param channel Sensor channel type
 * @return GSDML submodule identifier (module_ident + 1)
 */
static inline uint32_t gsdml_sensor_submodule_ident(sensor_channel_t channel) {
    return gsdml_sensor_module_ident(channel) + 1;
}

/**
 * @brief Get GSDML module identifier for an actuator type
 * @param type Actuator type (pump, valve, relay, etc.)
 * @return GSDML module identifier
 */
static inline uint32_t gsdml_actuator_module_ident(actuator_type_t type) {
    switch (type) {
        case ACTUATOR_TYPE_PUMP:
        case ACTUATOR_TYPE_PWM:
            return GSDML_MOD_ACTUATOR_PUMP;
        case ACTUATOR_TYPE_VALVE:
            return GSDML_MOD_ACTUATOR_VALVE;
        case ACTUATOR_TYPE_RELAY:
        case ACTUATOR_TYPE_LATCHING:
        case ACTUATOR_TYPE_MOMENTARY:
        default:
            return GSDML_MOD_ACTUATOR_GENERIC;
    }
}

/**
 * @brief Get GSDML submodule identifier for an actuator type
 * @param type Actuator type
 * @return GSDML submodule identifier (module_ident + 1)
 */
static inline uint32_t gsdml_actuator_submodule_ident(actuator_type_t type) {
    return gsdml_actuator_module_ident(type) + 1;
}

/**
 * @brief Get GSDML module identifier from sensor type string
 * @param sensor_type Sensor type string (e.g., "pH Sensor", "TDS Sensor")
 * @return GSDML module identifier
 *
 * Maps hardware/sensor type strings from database to GSDML module IDs.
 * Case-insensitive substring matching.
 */
static inline uint32_t gsdml_sensor_module_from_string(const char *sensor_type) {
    if (!sensor_type) return GSDML_MOD_SENSOR_GENERIC;

    /* pH sensor */
    if (strstr(sensor_type, "pH") || strstr(sensor_type, "ph") ||
        strstr(sensor_type, "PH")) {
        return GSDML_MOD_SENSOR_PH;
    }
    /* TDS / Conductivity */
    if (strstr(sensor_type, "TDS") || strstr(sensor_type, "tds") ||
        strstr(sensor_type, "Conductivity") || strstr(sensor_type, "conductivity")) {
        return GSDML_MOD_SENSOR_TDS;
    }
    /* Turbidity */
    if (strstr(sensor_type, "Turbidity") || strstr(sensor_type, "turbidity") ||
        strstr(sensor_type, "NTU")) {
        return GSDML_MOD_SENSOR_TURBIDITY;
    }
    /* Temperature */
    if (strstr(sensor_type, "Temp") || strstr(sensor_type, "temp") ||
        strstr(sensor_type, "DS18B20") || strstr(sensor_type, "DHT") ||
        strstr(sensor_type, "BME")) {
        return GSDML_MOD_SENSOR_TEMP;
    }
    /* Flow / Pressure */
    if (strstr(sensor_type, "Flow") || strstr(sensor_type, "flow") ||
        strstr(sensor_type, "Pressure") || strstr(sensor_type, "pressure")) {
        return GSDML_MOD_SENSOR_FLOW;
    }
    /* Level */
    if (strstr(sensor_type, "Level") || strstr(sensor_type, "level") ||
        strstr(sensor_type, "Float") || strstr(sensor_type, "HX711") ||
        strstr(sensor_type, "Distance") || strstr(sensor_type, "JSN")) {
        return GSDML_MOD_SENSOR_LEVEL;
    }

    return GSDML_MOD_SENSOR_GENERIC;
}

/**
 * @brief Get GSDML submodule identifier from sensor type string
 */
static inline uint32_t gsdml_sensor_submodule_from_string(const char *sensor_type) {
    return gsdml_sensor_module_from_string(sensor_type) + 1;
}

#endif /* GSDML_MODULES_H */
