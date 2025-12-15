/**
 * @file driver_ads1115.c
 * @brief ADS1115 16-bit ADC driver (I2C)
 */

#include "common.h"
#include "utils/logger.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>

#define ADS1115_DEFAULT_ADDR    0x48
#define ADS1115_REG_CONVERSION  0x00
#define ADS1115_REG_CONFIG      0x01

#define ADS1115_CONFIG_OS_SINGLE    0x8000
#define ADS1115_CONFIG_MUX_MASK     0x7000
#define ADS1115_CONFIG_PGA_MASK     0x0E00
#define ADS1115_CONFIG_MODE_SINGLE  0x0100
#define ADS1115_CONFIG_DR_128SPS    0x0080
#define ADS1115_CONFIG_COMP_QUE_DIS 0x0003

typedef enum {
    ADS1115_MUX_AIN0 = 0x4000,
    ADS1115_MUX_AIN1 = 0x5000,
    ADS1115_MUX_AIN2 = 0x6000,
    ADS1115_MUX_AIN3 = 0x7000
} ads1115_mux_t;

typedef enum {
    ADS1115_PGA_6144 = 0x0000,  // +/- 6.144V
    ADS1115_PGA_4096 = 0x0200,  // +/- 4.096V
    ADS1115_PGA_2048 = 0x0400,  // +/- 2.048V (default)
    ADS1115_PGA_1024 = 0x0600,  // +/- 1.024V
    ADS1115_PGA_0512 = 0x0800,  // +/- 0.512V
    ADS1115_PGA_0256 = 0x0A00   // +/- 0.256V
} ads1115_pga_t;

typedef struct {
    int fd;
    uint8_t address;
    int bus;
    ads1115_pga_t pga;
    bool initialized;
} ads1115_t;

static float pga_to_voltage_scale(ads1115_pga_t pga) {
    switch (pga) {
        case ADS1115_PGA_6144: return 6.144f / 32768.0f;
        case ADS1115_PGA_4096: return 4.096f / 32768.0f;
        case ADS1115_PGA_2048: return 2.048f / 32768.0f;
        case ADS1115_PGA_1024: return 1.024f / 32768.0f;
        case ADS1115_PGA_0512: return 0.512f / 32768.0f;
        case ADS1115_PGA_0256: return 0.256f / 32768.0f;
        default: return 2.048f / 32768.0f;
    }
}

static int i2c_write_word(int fd, uint8_t reg, uint16_t value) {
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (value >> 8) & 0xFF;
    buf[2] = value & 0xFF;
    return write(fd, buf, 3) == 3 ? 0 : -1;
}

static int i2c_read_word(int fd, uint8_t reg, uint16_t *value) {
    if (write(fd, &reg, 1) != 1) return -1;
    uint8_t buf[2];
    if (read(fd, buf, 2) != 2) return -1;
    *value = (buf[0] << 8) | buf[1];
    return 0;
}

result_t ads1115_init(ads1115_t *dev, int bus, uint8_t address, int gain) {
    CHECK_NULL(dev);
    
    memset(dev, 0, sizeof(*dev));
    dev->bus = bus;
    dev->address = address ? address : ADS1115_DEFAULT_ADDR;
    
    switch (gain) {
        case 0: dev->pga = ADS1115_PGA_6144; break;
        case 1: dev->pga = ADS1115_PGA_4096; break;
        case 2: dev->pga = ADS1115_PGA_2048; break;
        case 4: dev->pga = ADS1115_PGA_1024; break;
        case 8: dev->pga = ADS1115_PGA_0512; break;
        case 16: dev->pga = ADS1115_PGA_0256; break;
        default: dev->pga = ADS1115_PGA_4096; break;
    }
    
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    
    dev->fd = open(path, O_RDWR);
    if (dev->fd < 0) {
        LOG_ERROR("Failed to open I2C bus %d", bus);
        return RESULT_IO_ERROR;
    }
    
    if (ioctl(dev->fd, I2C_SLAVE, dev->address) < 0) {
        LOG_ERROR("Failed to set I2C address 0x%02X", dev->address);
        close(dev->fd);
        dev->fd = -1;
        return RESULT_IO_ERROR;
    }
    
    dev->initialized = true;
    LOG_INFO("ADS1115 initialized on bus %d, address 0x%02X", bus, dev->address);
    return RESULT_OK;
}

result_t ads1115_read_channel(ads1115_t *dev, int channel, float *voltage) {
    CHECK_NULL(dev);
    CHECK_NULL(voltage);
    
    if (!dev->initialized || dev->fd < 0) return RESULT_NOT_INITIALIZED;
    if (channel < 0 || channel > 3) return RESULT_INVALID_PARAM;
    
    ads1115_mux_t mux;
    switch (channel) {
        case 0: mux = ADS1115_MUX_AIN0; break;
        case 1: mux = ADS1115_MUX_AIN1; break;
        case 2: mux = ADS1115_MUX_AIN2; break;
        case 3: mux = ADS1115_MUX_AIN3; break;
        default: return RESULT_INVALID_PARAM;
    }
    
    uint16_t config = ADS1115_CONFIG_OS_SINGLE |
                      mux |
                      dev->pga |
                      ADS1115_CONFIG_MODE_SINGLE |
                      ADS1115_CONFIG_DR_128SPS |
                      ADS1115_CONFIG_COMP_QUE_DIS;
    
    if (i2c_write_word(dev->fd, ADS1115_REG_CONFIG, config) < 0) {
        LOG_ERROR("Failed to write config");
        return RESULT_IO_ERROR;
    }
    
    usleep(10000);  // Wait for conversion (128 SPS = ~8ms)
    
    uint16_t raw;
    if (i2c_read_word(dev->fd, ADS1115_REG_CONVERSION, &raw) < 0) {
        LOG_ERROR("Failed to read conversion");
        return RESULT_IO_ERROR;
    }
    
    int16_t signed_raw = (int16_t)raw;
    *voltage = signed_raw * pga_to_voltage_scale(dev->pga);
    
    return RESULT_OK;
}

result_t ads1115_read_raw(ads1115_t *dev, int channel, int16_t *raw) {
    CHECK_NULL(dev);
    CHECK_NULL(raw);
    
    float voltage;
    result_t r = ads1115_read_channel(dev, channel, &voltage);
    if (r != RESULT_OK) return r;
    
    *raw = (int16_t)(voltage / pga_to_voltage_scale(dev->pga));
    return RESULT_OK;
}

void ads1115_close(ads1115_t *dev) {
    if (dev && dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
        dev->initialized = false;
        LOG_DEBUG("ADS1115 closed");
    }
}

/* Driver interface wrapper */
typedef struct {
    ads1115_t device;
    int channel;
    float scale;
    float offset;
} ads1115_instance_t;

result_t driver_ads1115_init(void **handle, const char *address, int bus, int channel, int gain) {
    ads1115_instance_t *inst = calloc(1, sizeof(ads1115_instance_t));
    if (!inst) return RESULT_NO_MEMORY;
    
    uint8_t addr = address ? (uint8_t)strtol(address, NULL, 0) : ADS1115_DEFAULT_ADDR;
    
    result_t r = ads1115_init(&inst->device, bus, addr, gain);
    if (r != RESULT_OK) {
        free(inst);
        return r;
    }
    
    inst->channel = channel;
    inst->scale = 1.0f;
    inst->offset = 0.0f;
    
    *handle = inst;
    return RESULT_OK;
}

result_t driver_ads1115_read(void *handle, float *value) {
    CHECK_NULL(handle);
    CHECK_NULL(value);
    
    ads1115_instance_t *inst = (ads1115_instance_t *)handle;
    float voltage;
    
    result_t r = ads1115_read_channel(&inst->device, inst->channel, &voltage);
    if (r != RESULT_OK) return r;
    
    *value = voltage * inst->scale + inst->offset;
    return RESULT_OK;
}

result_t driver_ads1115_set_calibration(void *handle, float scale, float offset) {
    CHECK_NULL(handle);
    ads1115_instance_t *inst = (ads1115_instance_t *)handle;
    inst->scale = scale;
    inst->offset = offset;
    return RESULT_OK;
}

void driver_ads1115_close(void *handle) {
    if (handle) {
        ads1115_instance_t *inst = (ads1115_instance_t *)handle;
        ads1115_close(&inst->device);
        free(inst);
    }
}
