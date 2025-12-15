/**
 * @file driver_mcp3008.c
 * @brief MCP3008 10-bit ADC driver (SPI)
 */

#include "common.h"
#include "utils/logger.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>

#define MCP3008_CHANNELS    8
#define MCP3008_MAX_VALUE   1023
#define MCP3008_SPI_SPEED   1000000  // 1 MHz

typedef struct {
    int fd;
    int bus;
    int cs;
    uint32_t speed;
    float vref;
    bool initialized;
} mcp3008_t;

result_t mcp3008_init(mcp3008_t *dev, int bus, int cs, float vref) {
    CHECK_NULL(dev);
    
    memset(dev, 0, sizeof(*dev));
    dev->bus = bus;
    dev->cs = cs;
    dev->speed = MCP3008_SPI_SPEED;
    dev->vref = vref > 0 ? vref : 3.3f;
    
    char path[32];
    snprintf(path, sizeof(path), "/dev/spidev%d.%d", bus, cs);
    
    dev->fd = open(path, O_RDWR);
    if (dev->fd < 0) {
        LOG_ERROR("Failed to open SPI device %s", path);
        return RESULT_IO_ERROR;
    }
    
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    
    if (ioctl(dev->fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(dev->fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(dev->fd, SPI_IOC_WR_MAX_SPEED_HZ, &dev->speed) < 0) {
        LOG_ERROR("Failed to configure SPI");
        close(dev->fd);
        dev->fd = -1;
        return RESULT_IO_ERROR;
    }
    
    dev->initialized = true;
    LOG_INFO("MCP3008 initialized on SPI%d.%d, Vref=%.2fV", bus, cs, dev->vref);
    return RESULT_OK;
}

result_t mcp3008_read_channel(mcp3008_t *dev, int channel, uint16_t *raw) {
    CHECK_NULL(dev);
    CHECK_NULL(raw);
    
    if (!dev->initialized || dev->fd < 0) return RESULT_NOT_INITIALIZED;
    if (channel < 0 || channel >= MCP3008_CHANNELS) return RESULT_INVALID_PARAM;
    
    uint8_t tx[3] = {0x01, (0x80 | (channel << 4)), 0x00};
    uint8_t rx[3] = {0};
    
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = 3,
        .speed_hz = dev->speed,
        .bits_per_word = 8,
    };
    
    if (ioctl(dev->fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
        LOG_ERROR("SPI transfer failed");
        return RESULT_IO_ERROR;
    }
    
    *raw = ((rx[1] & 0x03) << 8) | rx[2];
    return RESULT_OK;
}

result_t mcp3008_read_voltage(mcp3008_t *dev, int channel, float *voltage) {
    CHECK_NULL(dev);
    CHECK_NULL(voltage);
    
    uint16_t raw;
    result_t r = mcp3008_read_channel(dev, channel, &raw);
    if (r != RESULT_OK) return r;
    
    *voltage = (raw / (float)MCP3008_MAX_VALUE) * dev->vref;
    return RESULT_OK;
}

void mcp3008_close(mcp3008_t *dev) {
    if (dev && dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
        dev->initialized = false;
        LOG_DEBUG("MCP3008 closed");
    }
}

/* Driver interface wrapper */
typedef struct {
    mcp3008_t device;
    int channel;
    float scale;
    float offset;
    float eng_min;
    float eng_max;
    int raw_min;
    int raw_max;
} mcp3008_instance_t;

result_t driver_mcp3008_init(void **handle, int bus, int cs, int channel, float vref) {
    mcp3008_instance_t *inst = calloc(1, sizeof(mcp3008_instance_t));
    if (!inst) return RESULT_NO_MEMORY;
    
    result_t r = mcp3008_init(&inst->device, bus, cs, vref);
    if (r != RESULT_OK) {
        free(inst);
        return r;
    }
    
    inst->channel = channel;
    inst->scale = 1.0f;
    inst->offset = 0.0f;
    inst->eng_min = 0.0f;
    inst->eng_max = vref;
    inst->raw_min = 0;
    inst->raw_max = MCP3008_MAX_VALUE;
    
    *handle = inst;
    return RESULT_OK;
}

result_t driver_mcp3008_read(void *handle, float *value) {
    CHECK_NULL(handle);
    CHECK_NULL(value);
    
    mcp3008_instance_t *inst = (mcp3008_instance_t *)handle;
    uint16_t raw;
    
    result_t r = mcp3008_read_channel(&inst->device, inst->channel, &raw);
    if (r != RESULT_OK) return r;
    
    // Linear interpolation from raw to engineering units
    float normalized = (float)(raw - inst->raw_min) / (float)(inst->raw_max - inst->raw_min);
    *value = inst->eng_min + normalized * (inst->eng_max - inst->eng_min);
    *value = *value * inst->scale + inst->offset;
    
    return RESULT_OK;
}

result_t driver_mcp3008_read_raw(void *handle, uint16_t *raw) {
    CHECK_NULL(handle);
    CHECK_NULL(raw);
    
    mcp3008_instance_t *inst = (mcp3008_instance_t *)handle;
    return mcp3008_read_channel(&inst->device, inst->channel, raw);
}

result_t driver_mcp3008_set_scaling(void *handle, int raw_min, int raw_max, float eng_min, float eng_max) {
    CHECK_NULL(handle);
    mcp3008_instance_t *inst = (mcp3008_instance_t *)handle;
    inst->raw_min = raw_min;
    inst->raw_max = raw_max;
    inst->eng_min = eng_min;
    inst->eng_max = eng_max;
    return RESULT_OK;
}

result_t driver_mcp3008_set_calibration(void *handle, float scale, float offset) {
    CHECK_NULL(handle);
    mcp3008_instance_t *inst = (mcp3008_instance_t *)handle;
    inst->scale = scale;
    inst->offset = offset;
    return RESULT_OK;
}

void driver_mcp3008_close(void *handle) {
    if (handle) {
        mcp3008_instance_t *inst = (mcp3008_instance_t *)handle;
        mcp3008_close(&inst->device);
        free(inst);
    }
}
