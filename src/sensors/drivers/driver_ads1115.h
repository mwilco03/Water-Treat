#ifndef DRIVER_ADS1115_H
#define DRIVER_ADS1115_H

#include "common.h"

typedef enum {
    ADS1115_GAIN_6144MV = 0,
    ADS1115_GAIN_4096MV = 1,
    ADS1115_GAIN_2048MV = 2,
    ADS1115_GAIN_1024MV = 4,
    ADS1115_GAIN_0512MV = 8,
    ADS1115_GAIN_0256MV = 16
} ads1115_gain_t;

typedef enum {
    ADS1115_SPS_8 = 8,
    ADS1115_SPS_16 = 16,
    ADS1115_SPS_32 = 32,
    ADS1115_SPS_64 = 64,
    ADS1115_SPS_128 = 128,
    ADS1115_SPS_250 = 250,
    ADS1115_SPS_475 = 475,
    ADS1115_SPS_860 = 860
} ads1115_sps_t;

typedef enum {
    ADS1115_PGA_6144 = 0x0000,
    ADS1115_PGA_4096 = 0x0200,
    ADS1115_PGA_2048 = 0x0400,
    ADS1115_PGA_1024 = 0x0600,
    ADS1115_PGA_0512 = 0x0800,
    ADS1115_PGA_0256 = 0x0A00
} ads1115_pga_t;

typedef struct {
    int fd;
    uint8_t address;
    int bus;
    ads1115_pga_t pga;
    bool initialized;
} ads1115_t;

result_t ads1115_init(ads1115_t *dev, int bus, uint8_t address, int gain);
result_t ads1115_read_channel(ads1115_t *dev, int channel, float *voltage);
result_t ads1115_read_raw(ads1115_t *dev, int channel, int16_t *raw);
void ads1115_close(ads1115_t *dev);

/* Convenience functions for sensor_instance.c compatibility */
static inline result_t ads1115_set_gain(ads1115_t *dev, ads1115_gain_t gain) {
    (void)dev; (void)gain; return RESULT_OK; /* Gain set during init */
}

static inline result_t ads1115_set_sample_rate(ads1115_t *dev, ads1115_sps_t sps) {
    (void)dev; (void)sps; return RESULT_OK; /* Not implemented */
}

/* Driver interface wrapper */
result_t driver_ads1115_init(void **handle, const char *address, int bus, int channel, int gain);
result_t driver_ads1115_read(void *handle, float *value);
result_t driver_ads1115_set_calibration(void *handle, float scale, float offset);
void driver_ads1115_close(void *handle);

#endif
