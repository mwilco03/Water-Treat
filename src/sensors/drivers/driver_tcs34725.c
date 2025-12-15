#include "driver_tcs34725.h"
#include "utils/logger.h"
#include <unistd.h>
#include <math.h>

// TCS34725 Registers
#define TCS34725_ADDRESS          0x29
#define TCS34725_COMMAND_BIT      0x80
#define TCS34725_ENABLE           0x00
#define TCS34725_ATIME            0x01
#define TCS34725_CONTROL          0x0F
#define TCS34725_ID               0x12
#define TCS34725_STATUS           0x13
#define TCS34725_CDATAL           0x14
#define TCS34725_CDATAH           0x15
#define TCS34725_RDATAL           0x16
#define TCS34725_RDATAH           0x17
#define TCS34725_GDATAL           0x18
#define TCS34725_GDATAH           0x19
#define TCS34725_BDATAL           0x1A
#define TCS34725_BDATAH           0x1B

#define TCS34725_ENABLE_PON       0x01
#define TCS34725_ENABLE_AEN       0x02

result_t tcs34725_init(tcs34725_device_t *dev, int bus, uint8_t address) {
    result_t result = i2c_open(&dev->i2c, bus, address);
    if (result != RESULT_OK) {
        return result;
    }
    
    // Read ID register to verify device
    uint8_t id;
    result = i2c_read_byte(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_ID, &id);
    if (result != RESULT_OK || (id != 0x44 && id != 0x4D)) {
        LOG_ERROR("TCS34725 not found (ID: 0x%02X)", id);
        i2c_close(&dev->i2c);
        return RESULT_ERROR;
    }
    
    dev->gain = TCS34725_GAIN_4X;
    dev->integration_time = TCS34725_INTEGRATIONTIME_154MS;
    dev->enabled = false;
    
    LOG_INFO("Initialized TCS34725 on I2C bus %d, address 0x%02X", bus, address);
    return RESULT_OK;
}

void tcs34725_destroy(tcs34725_device_t *dev) {
    if (dev->enabled) {
        tcs34725_disable(dev);
    }
    i2c_close(&dev->i2c);
}

result_t tcs34725_enable(tcs34725_device_t *dev) {
    // Power on
    result_t result = i2c_write_byte(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_ENABLE, 
                                     TCS34725_ENABLE_PON);
    if (result != RESULT_OK) {
        return result;
    }
    
    usleep(3000);  // Wait 3ms for power on
    
    // Enable ADC
    result = i2c_write_byte(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_ENABLE,
                           TCS34725_ENABLE_PON | TCS34725_ENABLE_AEN);
    if (result != RESULT_OK) {
        return result;
    }
    
    // Set integration time
    result = tcs34725_set_integration_time(dev, dev->integration_time);
    if (result != RESULT_OK) {
        return result;
    }
    
    // Set gain
    result = tcs34725_set_gain(dev, dev->gain);
    if (result != RESULT_OK) {
        return result;
    }
    
    dev->enabled = true;
    return RESULT_OK;
}

result_t tcs34725_disable(tcs34725_device_t *dev) {
    result_t result = i2c_write_byte(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_ENABLE, 0x00);
    dev->enabled = false;
    return result;
}

result_t tcs34725_set_gain(tcs34725_device_t *dev, tcs34725_gain_t gain) {
    dev->gain = gain;
    return i2c_write_byte(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_CONTROL, gain);
}

result_t tcs34725_set_integration_time(tcs34725_device_t *dev, tcs34725_integration_t time) {
    dev->integration_time = time;
    return i2c_write_byte(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_ATIME, time);
}

result_t tcs34725_read_raw(tcs34725_device_t *dev, tcs34725_data_t *data) {
    if (!dev->enabled) {
        result_t result = tcs34725_enable(dev);
        if (result != RESULT_OK) {
            return result;
        }
        
        // Wait for integration time
        usleep(200000);  // 200ms
    }
    
    // Read all color data
    uint16_t c, r, g, b;
    
    if (i2c_read_word(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_CDATAL, &c) != RESULT_OK ||
        i2c_read_word(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_RDATAL, &r) != RESULT_OK ||
        i2c_read_word(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_GDATAL, &g) != RESULT_OK ||
        i2c_read_word(&dev->i2c, TCS34725_COMMAND_BIT | TCS34725_BDATAL, &b) != RESULT_OK) {
        return RESULT_ERROR;
    }
    
    data->c = c;
    data->r = r;
    data->g = g;
    data->b = b;
    
    tcs34725_calculate_color_temperature(data);
    tcs34725_calculate_lux(data);
    
    return RESULT_OK;
}

result_t tcs34725_calculate_color_temperature(tcs34725_data_t *data) {
    // Calculate color temperature using McCamy's formula
    float x = (-0.14282f * data->r) + (1.54924f * data->g) + (-0.95641f * data->b);
    float y = (-0.32466f * data->r) + (1.57837f * data->g) + (-0.73191f * data->b);
    float z = (-0.68202f * data->r) + (0.77073f * data->g) + (0.56332f * data->b);
    
    float xc = x / (x + y + z);
    float yc = y / (x + y + z);
    
    float n = (xc - 0.3320f) / (0.1858f - yc);
    
    data->color_temp = (uint16_t)(449.0f * powf(n, 3) + 3525.0f * powf(n, 2) + 6823.3f * n + 5520.33f);
    
    return RESULT_OK;
}

result_t tcs34725_calculate_lux(tcs34725_data_t *data) {
    // Calculate lux using standard formula
    float illuminance = (-0.32466f * data->r) + (1.57837f * data->g) + (-0.73191f * data->b);
    data->lux = (uint16_t)(illuminance > 0 ? illuminance : 0);
    
    return RESULT_OK;
}