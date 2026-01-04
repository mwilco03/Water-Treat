/**
 * @file driver_bme280.c
 * @brief BME280/BMP280 Environmental sensor driver (I2C)
 *
 * Supports temperature, pressure, and humidity (BME280 only).
 * Uses shared I2C HAL from hw_interface.h for bus operations.
 */

#include "common.h"
#include "hardware/hw_interface.h"
#include "utils/logger.h"
#include <math.h>

/* ============================================================================
 * BME280 Register Definitions
 * ========================================================================== */

#define BME280_ADDR_PRIMARY     0x76
#define BME280_ADDR_SECONDARY   0x77

#define BME280_REG_CHIP_ID      0xD0
#define BME280_REG_CTRL_HUM     0xF2
#define BME280_REG_CTRL_MEAS    0xF4
#define BME280_REG_CONFIG       0xF5
#define BME280_REG_DATA         0xF7
#define BME280_REG_CALIB00      0x88
#define BME280_REG_CALIB26      0xE1

#define BME280_CHIP_ID          0x60
#define BMP280_CHIP_ID          0x58

typedef struct {
    uint16_t dig_T1;
    int16_t dig_T2;
    int16_t dig_T3;
    uint16_t dig_P1;
    int16_t dig_P2;
    int16_t dig_P3;
    int16_t dig_P4;
    int16_t dig_P5;
    int16_t dig_P6;
    int16_t dig_P7;
    int16_t dig_P8;
    int16_t dig_P9;
    uint8_t dig_H1;
    int16_t dig_H2;
    uint8_t dig_H3;
    int16_t dig_H4;
    int16_t dig_H5;
    int8_t dig_H6;
} bme280_calib_t;

typedef struct {
    i2c_device_t i2c;           /* Shared I2C HAL device */
    uint8_t chip_id;
    bool has_humidity;
    bme280_calib_t calib;
    int32_t t_fine;
    bool initialized;
} bme280_t;

static result_t read_calibration(bme280_t *dev) {
    uint8_t buf[26];

    if (i2c_read_bytes(&dev->i2c, BME280_REG_CALIB00, buf, 26) != RESULT_OK) {
        return RESULT_IO_ERROR;
    }
    
    dev->calib.dig_T1 = buf[0] | (buf[1] << 8);
    dev->calib.dig_T2 = buf[2] | (buf[3] << 8);
    dev->calib.dig_T3 = buf[4] | (buf[5] << 8);
    dev->calib.dig_P1 = buf[6] | (buf[7] << 8);
    dev->calib.dig_P2 = buf[8] | (buf[9] << 8);
    dev->calib.dig_P3 = buf[10] | (buf[11] << 8);
    dev->calib.dig_P4 = buf[12] | (buf[13] << 8);
    dev->calib.dig_P5 = buf[14] | (buf[15] << 8);
    dev->calib.dig_P6 = buf[16] | (buf[17] << 8);
    dev->calib.dig_P7 = buf[18] | (buf[19] << 8);
    dev->calib.dig_P8 = buf[20] | (buf[21] << 8);
    dev->calib.dig_P9 = buf[22] | (buf[23] << 8);
    dev->calib.dig_H1 = buf[25];
    
    if (dev->has_humidity) {
        uint8_t hum_buf[7];
        if (i2c_read_bytes(&dev->i2c, BME280_REG_CALIB26, hum_buf, 7) != RESULT_OK) {
            return RESULT_IO_ERROR;
        }
        dev->calib.dig_H2 = hum_buf[0] | (hum_buf[1] << 8);
        dev->calib.dig_H3 = hum_buf[2];
        dev->calib.dig_H4 = (hum_buf[3] << 4) | (hum_buf[4] & 0x0F);
        dev->calib.dig_H5 = (hum_buf[5] << 4) | (hum_buf[4] >> 4);
        dev->calib.dig_H6 = hum_buf[6];
    }
    
    return RESULT_OK;
}

result_t bme280_init(bme280_t *dev, int bus, uint8_t address) {
    CHECK_NULL(dev);

    memset(dev, 0, sizeof(*dev));

    /* Use shared I2C HAL for bus operations */
    uint8_t addr = address ? address : BME280_ADDR_PRIMARY;
    result_t result = i2c_open(&dev->i2c, bus, addr);
    if (result != RESULT_OK) {
        LOG_ERROR("BME280: Failed to open I2C bus %d, address 0x%02X", bus, addr);
        return result;
    }

    /* Read chip ID to verify device */
    uint8_t chip_id;
    if (i2c_read_byte(&dev->i2c, BME280_REG_CHIP_ID, &chip_id) != RESULT_OK) {
        LOG_ERROR("BME280: Failed to read chip ID");
        i2c_close(&dev->i2c);
        return RESULT_IO_ERROR;
    }

    dev->chip_id = chip_id;
    dev->has_humidity = (chip_id == BME280_CHIP_ID);

    if (chip_id != BME280_CHIP_ID && chip_id != BMP280_CHIP_ID) {
        LOG_ERROR("BME280: Unknown chip ID: 0x%02X", chip_id);
        i2c_close(&dev->i2c);
        return RESULT_NOT_FOUND;
    }

    /* Read calibration data */
    if (read_calibration(dev) != RESULT_OK) {
        i2c_close(&dev->i2c);
        return RESULT_IO_ERROR;
    }

    /* Configure sensor: humidity oversampling x1 */
    if (dev->has_humidity) {
        i2c_write_byte(&dev->i2c, BME280_REG_CTRL_HUM, 0x01);
    }

    /* Configure: temp x1, pressure x1, normal mode */
    i2c_write_byte(&dev->i2c, BME280_REG_CTRL_MEAS, 0x27);

    /* Config: standby 1000ms, filter off */
    i2c_write_byte(&dev->i2c, BME280_REG_CONFIG, 0xA0);

    dev->initialized = true;
    LOG_INFO("%s initialized on bus %d, address 0x%02X",
             dev->has_humidity ? "BME280" : "BMP280", bus, addr);
    return RESULT_OK;
}

result_t bme280_read(bme280_t *dev, float *temperature, float *pressure, float *humidity) {
    CHECK_NULL(dev);
    if (!dev->initialized) return RESULT_NOT_INITIALIZED;

    uint8_t data[8];
    if (i2c_read_bytes(&dev->i2c, BME280_REG_DATA, data, 8) != RESULT_OK) {
        return RESULT_IO_ERROR;
    }
    
    int32_t adc_P = (data[0] << 12) | (data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = (data[3] << 12) | (data[4] << 4) | (data[5] >> 4);
    int32_t adc_H = (data[6] << 8) | data[7];
    
    // Temperature compensation
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)dev->calib.dig_T1 << 1))) * 
                   ((int32_t)dev->calib.dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)dev->calib.dig_T1)) * 
                   ((adc_T >> 4) - ((int32_t)dev->calib.dig_T1))) >> 12) * 
                   ((int32_t)dev->calib.dig_T3)) >> 14;
    dev->t_fine = var1 + var2;
    
    if (temperature) {
        *temperature = (dev->t_fine * 5 + 128) >> 8;
        *temperature /= 100.0f;
    }
    
    // Pressure compensation
    if (pressure) {
        int64_t p_var1 = ((int64_t)dev->t_fine) - 128000;
        int64_t p_var2 = p_var1 * p_var1 * (int64_t)dev->calib.dig_P6;
        p_var2 = p_var2 + ((p_var1 * (int64_t)dev->calib.dig_P5) << 17);
        p_var2 = p_var2 + (((int64_t)dev->calib.dig_P4) << 35);
        p_var1 = ((p_var1 * p_var1 * (int64_t)dev->calib.dig_P3) >> 8) + 
                 ((p_var1 * (int64_t)dev->calib.dig_P2) << 12);
        p_var1 = (((((int64_t)1) << 47) + p_var1)) * ((int64_t)dev->calib.dig_P1) >> 33;
        
        if (p_var1 != 0) {
            int64_t p = 1048576 - adc_P;
            p = (((p << 31) - p_var2) * 3125) / p_var1;
            p_var1 = (((int64_t)dev->calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
            p_var2 = (((int64_t)dev->calib.dig_P8) * p) >> 19;
            p = ((p + p_var1 + p_var2) >> 8) + (((int64_t)dev->calib.dig_P7) << 4);
            *pressure = (float)p / 256.0f / 100.0f;  // hPa
        }
    }
    
    // Humidity compensation (BME280 only)
    if (humidity && dev->has_humidity) {
        int32_t h = dev->t_fine - 76800;
        h = (((((adc_H << 14) - (((int32_t)dev->calib.dig_H4) << 20) - 
            (((int32_t)dev->calib.dig_H5) * h)) + 16384) >> 15) * 
            (((((((h * ((int32_t)dev->calib.dig_H6)) >> 10) * 
            (((h * ((int32_t)dev->calib.dig_H3)) >> 11) + 32768)) >> 10) + 2097152) * 
            ((int32_t)dev->calib.dig_H2) + 8192) >> 14));
        h = h - (((((h >> 15) * (h >> 15)) >> 7) * ((int32_t)dev->calib.dig_H1)) >> 4);
        h = CLAMP(h, 0, 419430400);
        *humidity = (float)(h >> 12) / 1024.0f;
    } else if (humidity) {
        *humidity = 0.0f;
    }
    
    return RESULT_OK;
}

void bme280_close(bme280_t *dev) {
    if (dev && dev->initialized) {
        i2c_close(&dev->i2c);
        dev->initialized = false;
    }
}

/* Driver interface wrapper */
typedef enum {
    BME280_READ_TEMPERATURE = 0,
    BME280_READ_PRESSURE,
    BME280_READ_HUMIDITY
} bme280_reading_t;

typedef struct {
    bme280_t device;
    bme280_reading_t reading;
    float scale;
    float offset;
} bme280_instance_t;

result_t driver_bme280_init(void **handle, int bus, uint8_t address, int reading_type) {
    bme280_instance_t *inst = calloc(1, sizeof(bme280_instance_t));
    if (!inst) return RESULT_NO_MEMORY;
    
    result_t r = bme280_init(&inst->device, bus, address);
    if (r != RESULT_OK) {
        free(inst);
        return r;
    }
    
    inst->reading = (bme280_reading_t)reading_type;
    inst->scale = 1.0f;
    inst->offset = 0.0f;
    
    *handle = inst;
    return RESULT_OK;
}

result_t driver_bme280_read(void *handle, float *value) {
    CHECK_NULL(handle);
    CHECK_NULL(value);
    
    bme280_instance_t *inst = (bme280_instance_t *)handle;
    float temp, pres, hum;
    
    result_t r = bme280_read(&inst->device, &temp, &pres, &hum);
    if (r != RESULT_OK) return r;
    
    switch (inst->reading) {
        case BME280_READ_TEMPERATURE: *value = temp; break;
        case BME280_READ_PRESSURE: *value = pres; break;
        case BME280_READ_HUMIDITY: *value = hum; break;
        default: return RESULT_INVALID_PARAM;
    }
    
    *value = *value * inst->scale + inst->offset;
    return RESULT_OK;
}

result_t driver_bme280_read_all(void *handle, float *temperature, float *pressure, float *humidity) {
    CHECK_NULL(handle);
    bme280_instance_t *inst = (bme280_instance_t *)handle;
    return bme280_read(&inst->device, temperature, pressure, humidity);
}

void driver_bme280_close(void *handle) {
    if (handle) {
        bme280_instance_t *inst = (bme280_instance_t *)handle;
        bme280_close(&inst->device);
        free(inst);
    }
}
