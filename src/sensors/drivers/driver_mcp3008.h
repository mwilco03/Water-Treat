#ifndef DRIVER_MCP3008_H
#define DRIVER_MCP3008_H

#include "common.h"

typedef struct {
    int fd;
    int bus;
    int device;
    float vref;
    bool initialized;
} mcp3008_t;

result_t mcp3008_init(mcp3008_t *dev, int bus, int cs, float vref);
result_t mcp3008_read_channel(mcp3008_t *dev, int channel, uint16_t *raw);
result_t mcp3008_read_voltage(mcp3008_t *dev, int channel, float *voltage);
void mcp3008_close(mcp3008_t *dev);

/* Driver interface wrapper */
result_t driver_mcp3008_init(void **handle, int bus, int cs, int channel, float vref);
result_t driver_mcp3008_read(void *handle, float *value);
result_t driver_mcp3008_read_raw(void *handle, uint16_t *raw);
result_t driver_mcp3008_set_scaling(void *handle, int raw_min, int raw_max, float eng_min, float eng_max);
result_t driver_mcp3008_set_calibration(void *handle, float scale, float offset);
void driver_mcp3008_close(void *handle);

#endif
