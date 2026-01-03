/**
 * @file driver_ads1115.c
 * @brief ADS1115 16-bit ADC driver (I2C)
 *
 * Uses shared I2C HAL from hw_interface.h for bus operations.
 */

#include "common.h"
#include "hardware/hw_interface.h"
#include "utils/logger.h"
#include <unistd.h>

/* ============================================================================
 * ADS1115 Register Definitions
 * ========================================================================== */

#define ADS1115_DEFAULT_ADDR    0x48
#define ADS1115_REG_CONVERSION  0x00
#define ADS1115_REG_CONFIG      0x01

/* Config register bits */
#define ADS1115_CONFIG_OS_SINGLE    0x8000  /* Start single conversion */
#define ADS1115_CONFIG_MUX_MASK     0x7000
#define ADS1115_CONFIG_PGA_MASK     0x0E00
#define ADS1115_CONFIG_MODE_SINGLE  0x0100  /* Single-shot mode */
#define ADS1115_CONFIG_DR_128SPS    0x0080  /* 128 samples per second */
#define ADS1115_CONFIG_COMP_QUE_DIS 0x0003  /* Disable comparator */

/* ADC resolution: 16-bit signed = 32768 counts for positive full-scale */
#define ADS1115_FULL_SCALE_COUNTS   32768.0f

/* Conversion timing: calculate from sample rate with safety margin
 * At 128 SPS, conversion time = 1/128 = 7.8ms, add 25% margin */
#define ADS1115_SAMPLE_RATE_SPS     128
#define ADS1115_CONVERSION_TIME_US  ((1000000 / ADS1115_SAMPLE_RATE_SPS) * 125 / 100)

typedef enum {
    ADS1115_MUX_AIN0 = 0x4000,
    ADS1115_MUX_AIN1 = 0x5000,
    ADS1115_MUX_AIN2 = 0x6000,
    ADS1115_MUX_AIN3 = 0x7000
} ads1115_mux_t;

/* PGA (Programmable Gain Amplifier) settings with full-scale voltage ranges */
typedef enum {
    ADS1115_PGA_6144 = 0x0000,  /* +/- 6.144V (gain 2/3) */
    ADS1115_PGA_4096 = 0x0200,  /* +/- 4.096V (gain 1) */
    ADS1115_PGA_2048 = 0x0400,  /* +/- 2.048V (gain 2, default) */
    ADS1115_PGA_1024 = 0x0600,  /* +/- 1.024V (gain 4) */
    ADS1115_PGA_0512 = 0x0800,  /* +/- 0.512V (gain 8) */
    ADS1115_PGA_0256 = 0x0A00   /* +/- 0.256V (gain 16) */
} ads1115_pga_t;

typedef struct {
    i2c_device_t i2c;           /* Shared I2C HAL device */
    ads1115_pga_t pga;
    bool initialized;
} ads1115_t;

/* PGA full-scale voltage lookup table */
static const float pga_full_scale[] = {
    6.144f,  /* ADS1115_PGA_6144 */
    4.096f,  /* ADS1115_PGA_4096 */
    2.048f,  /* ADS1115_PGA_2048 */
    1.024f,  /* ADS1115_PGA_1024 */
    0.512f,  /* ADS1115_PGA_0512 */
    0.256f   /* ADS1115_PGA_0256 */
};

static float pga_to_voltage_scale(ads1115_pga_t pga) {
    int index = (pga >> 9) & 0x07;  /* Extract PGA bits */
    if (index > 5) index = 2;        /* Default to 2.048V */
    return pga_full_scale[index] / ADS1115_FULL_SCALE_COUNTS;
}

result_t ads1115_init(ads1115_t *dev, int bus, uint8_t address, int gain) {
    CHECK_NULL(dev);

    memset(dev, 0, sizeof(*dev));

    /* Map gain parameter to PGA setting */
    switch (gain) {
        case 0:  dev->pga = ADS1115_PGA_6144; break;
        case 1:  dev->pga = ADS1115_PGA_4096; break;
        case 2:  dev->pga = ADS1115_PGA_2048; break;
        case 4:  dev->pga = ADS1115_PGA_1024; break;
        case 8:  dev->pga = ADS1115_PGA_0512; break;
        case 16: dev->pga = ADS1115_PGA_0256; break;
        default: dev->pga = ADS1115_PGA_4096; break;
    }

    /* Use shared I2C HAL for bus operations */
    uint8_t addr = address ? address : ADS1115_DEFAULT_ADDR;
    result_t result = i2c_open(&dev->i2c, bus, addr);
    if (result != RESULT_OK) {
        LOG_ERROR("ADS1115: Failed to open I2C bus %d, address 0x%02X", bus, addr);
        return result;
    }

    dev->initialized = true;
    LOG_INFO("ADS1115 initialized on bus %d, address 0x%02X", bus, addr);
    return RESULT_OK;
}

result_t ads1115_read_channel(ads1115_t *dev, int channel, float *voltage) {
    CHECK_NULL(dev);
    CHECK_NULL(voltage);

    if (!dev->initialized) return RESULT_NOT_INITIALIZED;
    if (channel < 0 || channel > 3) return RESULT_INVALID_PARAM;

    /* Select input channel via MUX */
    ads1115_mux_t mux;
    switch (channel) {
        case 0: mux = ADS1115_MUX_AIN0; break;
        case 1: mux = ADS1115_MUX_AIN1; break;
        case 2: mux = ADS1115_MUX_AIN2; break;
        case 3: mux = ADS1115_MUX_AIN3; break;
        default: return RESULT_INVALID_PARAM;
    }

    /* Build config register value */
    uint16_t config = ADS1115_CONFIG_OS_SINGLE |
                      mux |
                      dev->pga |
                      ADS1115_CONFIG_MODE_SINGLE |
                      ADS1115_CONFIG_DR_128SPS |
                      ADS1115_CONFIG_COMP_QUE_DIS;

    /* Write config to start conversion (using shared I2C HAL) */
    result_t result = i2c_write_word(&dev->i2c, ADS1115_REG_CONFIG, config);
    if (result != RESULT_OK) {
        LOG_ERROR("ADS1115: Failed to write config register");
        return result;
    }

    /* Wait for conversion (calculated from sample rate) */
    usleep(ADS1115_CONVERSION_TIME_US);

    /* Read conversion result (using shared I2C HAL) */
    uint16_t raw;
    result = i2c_read_word(&dev->i2c, ADS1115_REG_CONVERSION, &raw);
    if (result != RESULT_OK) {
        LOG_ERROR("ADS1115: Failed to read conversion register");
        return result;
    }

    /* Convert to voltage: signed 16-bit value * scale factor */
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
    if (dev && dev->initialized) {
        i2c_close(&dev->i2c);
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
