/**
 * @file driver_common.h
 * @brief Common sensor driver infrastructure
 *
 * Provides a unified interface for all sensor drivers, reducing boilerplate
 * and enabling consistent behavior across driver implementations.
 *
 * Usage:
 *   1. Include this header in driver implementations
 *   2. Define driver_ops_t for your driver
 *   3. Use DRIVER_INSTANCE_FIELDS in your instance struct
 *   4. Call driver_instance_* helpers for common operations
 */

#ifndef DRIVER_COMMON_H
#define DRIVER_COMMON_H

#include "common.h"

/**
 * @brief Driver operation function pointers
 *
 * All drivers implement this interface. Functions may be NULL if not supported.
 */
typedef struct driver_ops {
    /** Read current sensor value */
    result_t (*read)(void *handle, float *value);

    /** Close and cleanup driver resources */
    void (*close)(void *handle);

    /** Set calibration parameters (optional) */
    result_t (*set_calibration)(void *handle, float scale, float offset);

    /** Get driver name for logging */
    const char *name;
} driver_ops_t;

/**
 * @brief Common fields for all driver instances
 *
 * Embed this at the START of driver-specific instance structs:
 *
 *   typedef struct {
 *       DRIVER_INSTANCE_FIELDS;
 *       my_device_t device;
 *       int channel;
 *   } my_driver_instance_t;
 */
#define DRIVER_INSTANCE_FIELDS \
    const driver_ops_t *ops;   \
    float scale;               \
    float offset

/**
 * @brief Base driver instance structure
 *
 * Can be used directly or extended by driver-specific instances.
 */
typedef struct {
    DRIVER_INSTANCE_FIELDS;
} driver_instance_base_t;

/**
 * @brief Initialize common driver instance fields
 *
 * Call this at the start of driver_xxx_init() after allocating the instance.
 *
 * @param inst Pointer to driver instance (must have DRIVER_INSTANCE_FIELDS)
 * @param ops  Pointer to driver operations table
 */
static inline void driver_instance_init_base(void *inst, const driver_ops_t *ops) {
    driver_instance_base_t *base = (driver_instance_base_t *)inst;
    base->ops = ops;
    base->scale = 1.0f;
    base->offset = 0.0f;
}

/**
 * @brief Apply calibration to a raw value
 *
 * @param inst  Pointer to driver instance
 * @param value Raw sensor value
 * @return Calibrated value: (value * scale) + offset
 */
static inline float driver_apply_calibration(const void *inst, float value) {
    const driver_instance_base_t *base = (const driver_instance_base_t *)inst;
    return (value * base->scale) + base->offset;
}

/**
 * @brief Generic set_calibration implementation
 *
 * Can be used directly as ops->set_calibration for drivers with standard calibration.
 */
static inline result_t driver_set_calibration_generic(void *handle, float scale, float offset) {
    if (!handle) return RESULT_INVALID_PARAM;
    driver_instance_base_t *base = (driver_instance_base_t *)handle;
    base->scale = scale;
    base->offset = offset;
    return RESULT_OK;
}

/**
 * @brief Read from driver using ops table
 *
 * @param handle Driver instance handle
 * @param value  Output value pointer
 * @return RESULT_OK on success
 */
static inline result_t driver_read(void *handle, float *value) {
    if (!handle || !value) return RESULT_INVALID_PARAM;
    driver_instance_base_t *base = (driver_instance_base_t *)handle;
    if (!base->ops || !base->ops->read) return RESULT_NOT_SUPPORTED;
    return base->ops->read(handle, value);
}

/**
 * @brief Close driver using ops table
 *
 * @param handle Driver instance handle (will be freed)
 */
static inline void driver_close(void *handle) {
    if (!handle) return;
    driver_instance_base_t *base = (driver_instance_base_t *)handle;
    if (base->ops && base->ops->close) {
        base->ops->close(handle);
    }
}

/**
 * @brief Get driver name
 *
 * @param handle Driver instance handle
 * @return Driver name string or "unknown"
 */
static inline const char *driver_get_name(void *handle) {
    if (!handle) return "null";
    driver_instance_base_t *base = (driver_instance_base_t *)handle;
    if (!base->ops || !base->ops->name) return "unknown";
    return base->ops->name;
}

/*
 * Driver type enumeration
 *
 * Used by sensor_instance to track which driver is in use.
 * Each driver registers here when added to the system.
 */
typedef enum {
    DRIVER_TYPE_NONE = 0,

    /* Physical sensor drivers */
    DRIVER_TYPE_DS18B20,
    DRIVER_TYPE_DHT22,
    DRIVER_TYPE_BME280,
    DRIVER_TYPE_HX711,
    DRIVER_TYPE_JSN_SR04T,
    DRIVER_TYPE_TCS34725,
    DRIVER_TYPE_FLOAT_SWITCH,

    /* ADC drivers */
    DRIVER_TYPE_ADS1115,
    DRIVER_TYPE_MCP3008,

    /* Network drivers */
    DRIVER_TYPE_WEB_POLL,

    DRIVER_TYPE_COUNT
} driver_type_t;

/**
 * @brief Driver registration entry
 *
 * Allows lookup of driver ops by type or name.
 */
typedef struct {
    driver_type_t type;
    const char *name;
    const driver_ops_t *ops;
} driver_registry_entry_t;

/**
 * @brief Get driver ops by type
 *
 * @param type Driver type enum
 * @return Pointer to driver ops or NULL if not found
 */
const driver_ops_t *driver_get_ops_by_type(driver_type_t type);

/**
 * @brief Get driver ops by name
 *
 * @param name Driver name (case-insensitive)
 * @return Pointer to driver ops or NULL if not found
 */
const driver_ops_t *driver_get_ops_by_name(const char *name);

/**
 * @brief Get driver type from name
 *
 * @param name Driver name (case-insensitive)
 * @return Driver type or DRIVER_TYPE_NONE if not found
 */
driver_type_t driver_get_type_by_name(const char *name);

#endif /* DRIVER_COMMON_H */
