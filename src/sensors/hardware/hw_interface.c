#include "hw_interface.h"
#include "utils/logger.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// I2C IMPLEMENTATION
// ============================================================================

result_t i2c_open(i2c_device_t *dev, int bus, uint8_t address) {
    char device_path[64];
    SAFE_SNPRINTF(device_path, sizeof(device_path), "/dev/i2c-%d", bus);
    
    dev->bus = bus;
    dev->address = address;
    dev->fd = open(device_path, O_RDWR);
    
    if (dev->fd < 0) {
        LOG_ERROR("Failed to open I2C bus %d: %s", bus, strerror(errno));
        return RESULT_ERROR;
    }
    
    if (ioctl(dev->fd, I2C_SLAVE, address) < 0) {
        LOG_ERROR("Failed to set I2C slave address 0x%02X: %s", address, strerror(errno));
        close(dev->fd);
        return RESULT_ERROR;
    }
    
    LOG_DEBUG("Opened I2C device: bus=%d, address=0x%02X", bus, address);
    return RESULT_OK;
}

void i2c_close(i2c_device_t *dev) {
    if (dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
}

result_t i2c_read_byte(i2c_device_t *dev, uint8_t reg, uint8_t *value) {
    if (write(dev->fd, &reg, 1) != 1) {
        LOG_ERROR("I2C write register failed");
        return RESULT_ERROR;
    }
    
    if (read(dev->fd, value, 1) != 1) {
        LOG_ERROR("I2C read byte failed");
        return RESULT_ERROR;
    }
    
    return RESULT_OK;
}

result_t i2c_read_word(i2c_device_t *dev, uint8_t reg, uint16_t *value) {
    uint8_t buffer[2];
    
    if (write(dev->fd, &reg, 1) != 1) {
        LOG_ERROR("I2C write register failed");
        return RESULT_ERROR;
    }
    
    if (read(dev->fd, buffer, 2) != 2) {
        LOG_ERROR("I2C read word failed");
        return RESULT_ERROR;
    }
    
    // Most I2C devices use big-endian
    *value = (buffer[0] << 8) | buffer[1];
    
    return RESULT_OK;
}

result_t i2c_read_bytes(i2c_device_t *dev, uint8_t reg, uint8_t *buffer, size_t len) {
    if (write(dev->fd, &reg, 1) != 1) {
        LOG_ERROR("I2C write register failed");
        return RESULT_ERROR;
    }
    
    if (read(dev->fd, buffer, len) != (ssize_t)len) {
        LOG_ERROR("I2C read bytes failed");
        return RESULT_ERROR;
    }
    
    return RESULT_OK;
}

result_t i2c_write_byte(i2c_device_t *dev, uint8_t reg, uint8_t value) {
    uint8_t buffer[2] = {reg, value};
    
    if (write(dev->fd, buffer, 2) != 2) {
        LOG_ERROR("I2C write byte failed");
        return RESULT_ERROR;
    }
    
    return RESULT_OK;
}

result_t i2c_write_word(i2c_device_t *dev, uint8_t reg, uint16_t value) {
    uint8_t buffer[3] = {reg, (value >> 8) & 0xFF, value & 0xFF};
    
    if (write(dev->fd, buffer, 3) != 3) {
        LOG_ERROR("I2C write word failed");
        return RESULT_ERROR;
    }
    
    return RESULT_OK;
}

// ============================================================================
// SPI IMPLEMENTATION
// ============================================================================

result_t spi_open(spi_device_t *dev, int bus, int device) {
    char device_path[64];
    SAFE_SNPRINTF(device_path, sizeof(device_path), "/dev/spidev%d.%d", bus, device);
    
    dev->bus = bus;
    dev->device = device;
    dev->speed = 1000000;  // 1 MHz default
    dev->mode = SPI_MODE_0;
    dev->bits_per_word = 8;
    
    dev->fd = open(device_path, O_RDWR);
    
    if (dev->fd < 0) {
        LOG_ERROR("Failed to open SPI device %s: %s", device_path, strerror(errno));
        return RESULT_ERROR;
    }
    
    // Set SPI mode
    if (ioctl(dev->fd, SPI_IOC_WR_MODE, &dev->mode) < 0) {
        LOG_ERROR("Failed to set SPI mode");
        close(dev->fd);
        return RESULT_ERROR;
    }
    
    // Set bits per word
    if (ioctl(dev->fd, SPI_IOC_WR_BITS_PER_WORD, &dev->bits_per_word) < 0) {
        LOG_ERROR("Failed to set SPI bits per word");
        close(dev->fd);
        return RESULT_ERROR;
    }
    
    // Set max speed
    if (ioctl(dev->fd, SPI_IOC_WR_MAX_SPEED_HZ, &dev->speed) < 0) {
        LOG_ERROR("Failed to set SPI speed");
        close(dev->fd);
        return RESULT_ERROR;
    }
    
    LOG_DEBUG("Opened SPI device: bus=%d, device=%d", bus, device);
    return RESULT_OK;
}

void spi_close(spi_device_t *dev) {
    if (dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
}

result_t spi_transfer(spi_device_t *dev, uint8_t *tx_data, uint8_t *rx_data, size_t len) {
    struct spi_ioc_transfer transfer = {
        .tx_buf = (unsigned long)tx_data,
        .rx_buf = (unsigned long)rx_data,
        .len = len,
        .speed_hz = dev->speed,
        .bits_per_word = dev->bits_per_word,
    };
    
    if (ioctl(dev->fd, SPI_IOC_MESSAGE(1), &transfer) < 0) {
        LOG_ERROR("SPI transfer failed: %s", strerror(errno));
        return RESULT_ERROR;
    }
    
    return RESULT_OK;
}

result_t spi_set_mode(spi_device_t *dev, uint8_t mode) {
    dev->mode = mode;
    
    if (ioctl(dev->fd, SPI_IOC_WR_MODE, &dev->mode) < 0) {
        LOG_ERROR("Failed to set SPI mode");
        return RESULT_ERROR;
    }
    
    return RESULT_OK;
}

result_t spi_set_speed(spi_device_t *dev, uint32_t speed) {
    dev->speed = speed;
    
    if (ioctl(dev->fd, SPI_IOC_WR_MAX_SPEED_HZ, &dev->speed) < 0) {
        LOG_ERROR("Failed to set SPI speed");
        return RESULT_ERROR;
    }
    
    return RESULT_OK;
}

// ============================================================================
// GPIO IMPLEMENTATION
// ============================================================================

result_t hwif_gpio_export(gpio_pin_t *pin, int pin_number) {
    char path[128];
    
    pin->pin = pin_number;
    pin->exported = false;
    pin->value_fd = -1;
    
    // Check if already exported
    SAFE_SNPRINTF(path, sizeof(path), "/sys/class/gpio/gpio%d", pin_number);
    if (access(path, F_OK) == 0) {
        pin->exported = true;
    } else {
        // Export the pin
        int export_fd = open("/sys/class/gpio/export", O_WRONLY);
        if (export_fd < 0) {
            LOG_ERROR("Failed to open GPIO export");
            return RESULT_ERROR;
        }
        
        char pin_str[8];
        SAFE_SNPRINTF(pin_str, sizeof(pin_str), "%d", pin_number);
        
        if (write(export_fd, pin_str, strlen(pin_str)) < 0) {
            LOG_ERROR("Failed to export GPIO %d", pin_number);
            close(export_fd);
            return RESULT_ERROR;
        }
        
        close(export_fd);
        pin->exported = true;
        
        // Wait for sysfs to settle
        usleep(100000);  // 100ms
    }
    
    // Open value file
    SAFE_SNPRINTF(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin_number);
    pin->value_fd = open(path, O_RDWR);
    
    if (pin->value_fd < 0) {
        LOG_ERROR("Failed to open GPIO value file");
        return RESULT_ERROR;
    }
    
    LOG_DEBUG("Exported GPIO pin %d", pin_number);
    return RESULT_OK;
}

void hwif_gpio_unexport(gpio_pin_t *pin) {
    if (pin->value_fd >= 0) {
        close(pin->value_fd);
        pin->value_fd = -1;
    }
    
    if (pin->exported) {
        int unexport_fd = open("/sys/class/gpio/unexport", O_WRONLY);
        if (unexport_fd >= 0) {
            char pin_str[8];
            SAFE_SNPRINTF(pin_str, sizeof(pin_str), "%d", pin->pin);
            ssize_t written __attribute__((unused)) = write(unexport_fd, pin_str, strlen(pin_str));
            close(unexport_fd);
        }
        pin->exported = false;
    }
}

result_t hwif_gpio_set_direction(gpio_pin_t *pin, gpio_direction_t dir) {
    char path[128];
    SAFE_SNPRINTF(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin->pin);
    
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        LOG_ERROR("Failed to open GPIO direction file");
        return RESULT_ERROR;
    }
    
    const char *dir_str = (dir == GPIO_DIR_IN) ? "in" : "out";
    if (write(fd, dir_str, strlen(dir_str)) < 0) {
        LOG_ERROR("Failed to set GPIO direction");
        close(fd);
        return RESULT_ERROR;
    }
    
    close(fd);
    return RESULT_OK;
}

result_t hwif_gpio_set_edge(gpio_pin_t *pin, gpio_edge_t edge) {
    char path[128];
    SAFE_SNPRINTF(path, sizeof(path), "/sys/class/gpio/gpio%d/edge", pin->pin);
    
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        LOG_ERROR("Failed to open GPIO edge file");
        return RESULT_ERROR;
    }
    
    const char *edge_str;
    switch (edge) {
        case GPIO_EDGE_RISING:  edge_str = "rising"; break;
        case GPIO_EDGE_FALLING: edge_str = "falling"; break;
        case GPIO_EDGE_BOTH:    edge_str = "both"; break;
        default:                edge_str = "none"; break;
    }
    
    if (write(fd, edge_str, strlen(edge_str)) < 0) {
        LOG_ERROR("Failed to set GPIO edge");
        close(fd);
        return RESULT_ERROR;
    }
    
    close(fd);
    return RESULT_OK;
}

result_t hwif_gpio_read(gpio_pin_t *pin, bool *value) {
    char buf[4];
    
    if (lseek(pin->value_fd, 0, SEEK_SET) < 0) {
        LOG_ERROR("GPIO lseek failed");
        return RESULT_ERROR;
    }
    
    if (read(pin->value_fd, buf, sizeof(buf)) < 0) {
        LOG_ERROR("GPIO read failed");
        return RESULT_ERROR;
    }
    
    *value = (buf[0] == '1');
    return RESULT_OK;
}

result_t hwif_gpio_write(gpio_pin_t *pin, bool value) {
    const char *val_str = value ? "1" : "0";
    
    if (lseek(pin->value_fd, 0, SEEK_SET) < 0) {
        LOG_ERROR("GPIO lseek failed");
        return RESULT_ERROR;
    }
    
    if (write(pin->value_fd, val_str, 1) < 0) {
        LOG_ERROR("GPIO write failed");
        return RESULT_ERROR;
    }
    
    return RESULT_OK;
}

result_t hwif_gpio_wait_for_edge(gpio_pin_t *pin, int timeout_ms) {
    struct pollfd pfd;
    char buf[4];

    // Clear any pending interrupt
    lseek(pin->value_fd, 0, SEEK_SET);
    ssize_t bytes_read __attribute__((unused)) = read(pin->value_fd, buf, sizeof(buf));
    
    pfd.fd = pin->value_fd;
    pfd.events = POLLPRI | POLLERR;
    pfd.revents = 0;
    
    int ret = poll(&pfd, 1, timeout_ms);
    
    if (ret < 0) {
        LOG_ERROR("GPIO poll failed");
        return RESULT_ERROR;
    } else if (ret == 0) {
        return RESULT_TIMEOUT;
    }
    
    return RESULT_OK;
}

// ============================================================================
// 1-WIRE IMPLEMENTATION
// ============================================================================

result_t onewire_scan(onewire_device_t **devices, int *count) {
    const char *base_path = "/sys/bus/w1/devices";
    DIR *dir = opendir(base_path);
    
    if (!dir) {
        LOG_ERROR("Failed to open 1-Wire directory");
        return RESULT_ERROR;
    }
    
    // Count devices first
    int device_count = 0;
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL) {
        // 1-Wire devices start with family code (e.g., "28-" for DS18B20)
        if (entry->d_name[0] != '.' && strchr(entry->d_name, '-') != NULL) {
            device_count++;
        }
    }
    
    if (device_count == 0) {
        closedir(dir);
        *devices = NULL;
        *count = 0;
        return RESULT_OK;
    }
    
    // Allocate device array
    *devices = calloc(device_count, sizeof(onewire_device_t));
    if (!*devices) {
        closedir(dir);
        return RESULT_ERROR;
    }
    
    // Populate device array
    rewinddir(dir);
    int idx = 0;
    
    while ((entry = readdir(dir)) != NULL && idx < device_count) {
        if (entry->d_name[0] != '.' && strchr(entry->d_name, '-') != NULL) {
            SAFE_STRNCPY((*devices)[idx].device_id, entry->d_name, 
                        sizeof((*devices)[idx].device_id));
            
            SAFE_SNPRINTF((*devices)[idx].device_path, sizeof((*devices)[idx].device_path),
                         "%s/%s/w1_slave", base_path, entry->d_name);
            
            idx++;
        }
    }
    
    closedir(dir);
    *count = idx;
    
    LOG_DEBUG("Found %d 1-Wire devices", idx);
    return RESULT_OK;
}

result_t onewire_read_temperature(const char *device_id, float *temperature) {
    char path[MAX_PATH_LEN];
    SAFE_SNPRINTF(path, sizeof(path), "/sys/bus/w1/devices/%s/w1_slave", device_id);
    
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_ERROR("Failed to open 1-Wire device %s", device_id);
        return RESULT_ERROR;
    }
    
    char line1[128], line2[128];
    if (!fgets(line1, sizeof(line1), fp) || !fgets(line2, sizeof(line2), fp)) {
        LOG_ERROR("Failed to read 1-Wire data");
        fclose(fp);
        return RESULT_ERROR;
    }
    
    fclose(fp);
    
    // Check CRC
    if (strstr(line1, "YES") == NULL) {
        LOG_ERROR("1-Wire CRC check failed");
        return RESULT_ERROR;
    }
    
    // Parse temperature
    char *temp_str = strstr(line2, "t=");
    if (!temp_str) {
        LOG_ERROR("Failed to parse 1-Wire temperature");
        return RESULT_ERROR;
    }
    
    int temp_raw = atoi(temp_str + 2);
    *temperature = temp_raw / 1000.0f;
    
    return RESULT_OK;
}