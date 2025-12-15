/**
 * @file i2c_hal.h
 * @brief I2C Hardware Abstraction Layer
 *
 * Platform-independent I2C interface.
 */

#ifndef I2C_HAL_H
#define I2C_HAL_H

#include "common.h"

/* ============================================================================
 * I2C Instance
 * ========================================================================== */

typedef struct i2c_bus i2c_bus_t;

struct i2c_bus {
    int bus_number;
    int fd;
    bool initialized;
};

/* ============================================================================
 * I2C Functions
 * ========================================================================== */

/**
 * Open an I2C bus
 */
result_t i2c_open(i2c_bus_t **bus, int bus_number);
void i2c_close(i2c_bus_t *bus);

/**
 * Read/Write operations
 */
result_t i2c_write(i2c_bus_t *bus, uint8_t addr, const uint8_t *data, size_t len);
result_t i2c_read(i2c_bus_t *bus, uint8_t addr, uint8_t *data, size_t len);

/**
 * Register operations (common pattern)
 */
result_t i2c_write_reg(i2c_bus_t *bus, uint8_t addr, uint8_t reg, uint8_t value);
result_t i2c_read_reg(i2c_bus_t *bus, uint8_t addr, uint8_t reg, uint8_t *value);

result_t i2c_write_reg16(i2c_bus_t *bus, uint8_t addr, uint8_t reg, uint16_t value);
result_t i2c_read_reg16(i2c_bus_t *bus, uint8_t addr, uint8_t reg, uint16_t *value);

/**
 * Bulk register read (for multi-byte sensors)
 */
result_t i2c_read_regs(i2c_bus_t *bus, uint8_t addr, uint8_t start_reg,
                       uint8_t *data, size_t len);

/**
 * Check if device is present
 */
result_t i2c_probe(i2c_bus_t *bus, uint8_t addr, bool *present);

/**
 * Scan bus for devices
 */
result_t i2c_scan(i2c_bus_t *bus, uint8_t *addresses, int max_count, int *found);

#endif /* I2C_HAL_H */
