#ifndef HW_INTERFACE_H
#define HW_INTERFACE_H

#include "common.h"

// ============================================================================
// I2C TYPES AND FUNCTIONS
// ============================================================================

typedef struct {
    int bus;
    uint8_t address;
    int fd;
} i2c_device_t;

result_t i2c_open(i2c_device_t *dev, int bus, uint8_t address);
void i2c_close(i2c_device_t *dev);
result_t i2c_read_byte(i2c_device_t *dev, uint8_t reg, uint8_t *value);
result_t i2c_read_word(i2c_device_t *dev, uint8_t reg, uint16_t *value);
result_t i2c_read_bytes(i2c_device_t *dev, uint8_t reg, uint8_t *buffer, size_t len);
result_t i2c_write_byte(i2c_device_t *dev, uint8_t reg, uint8_t value);
result_t i2c_write_word(i2c_device_t *dev, uint8_t reg, uint16_t value);

// ============================================================================
// SPI TYPES AND FUNCTIONS
// ============================================================================

typedef struct {
    int bus;
    int device;
    uint32_t speed;
    uint8_t mode;
    uint8_t bits_per_word;
    int fd;
} spi_device_t;

result_t spi_open(spi_device_t *dev, int bus, int device);
void spi_close(spi_device_t *dev);
result_t spi_transfer(spi_device_t *dev, uint8_t *tx_data, uint8_t *rx_data, size_t len);
result_t spi_set_mode(spi_device_t *dev, uint8_t mode);
result_t spi_set_speed(spi_device_t *dev, uint32_t speed);

// ============================================================================
// GPIO TYPES AND FUNCTIONS
// ============================================================================

typedef enum {
    GPIO_DIR_IN = 0,
    GPIO_DIR_OUT
} gpio_direction_t;

typedef enum {
    GPIO_EDGE_NONE = 0,
    GPIO_EDGE_RISING,
    GPIO_EDGE_FALLING,
    GPIO_EDGE_BOTH
} gpio_edge_t;

typedef struct {
    int pin;
    bool exported;
    int value_fd;
} gpio_pin_t;

result_t hwif_gpio_export(gpio_pin_t *pin, int pin_number);
void hwif_gpio_unexport(gpio_pin_t *pin);
result_t hwif_gpio_set_direction(gpio_pin_t *pin, gpio_direction_t dir);
result_t hwif_gpio_set_edge(gpio_pin_t *pin, gpio_edge_t edge);
result_t hwif_gpio_read(gpio_pin_t *pin, bool *value);
result_t hwif_gpio_write(gpio_pin_t *pin, bool value);
result_t hwif_gpio_wait_for_edge(gpio_pin_t *pin, int timeout_ms);

// ============================================================================
// 1-WIRE TYPES AND FUNCTIONS
// ============================================================================

typedef struct {
    char device_id[32];
    char device_path[MAX_PATH_LEN];
} onewire_device_t;

result_t onewire_scan(onewire_device_t **devices, int *count);
result_t onewire_read_temperature(const char *device_id, float *temperature);

#endif /* HW_INTERFACE_H */
