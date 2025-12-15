#ifndef SENSOR_INSTANCE_H
#define SENSOR_INSTANCE_H

#include "common.h"
#include "db/database.h"
#include "db/db_modules.h"

typedef enum {
    SENSOR_INSTANCE_PHYSICAL,
    SENSOR_INSTANCE_ADC,
    SENSOR_INSTANCE_CALCULATED,
    SENSOR_INSTANCE_WEB_POLL,
    SENSOR_INSTANCE_STATIC
} sensor_instance_type_t;

typedef struct {
    int id;
    int slot;
    char name[MAX_NAME_LEN];
    sensor_instance_type_t type;

    void *driver_handle;
    void *driver_ctx;

    float current_value;
    char status[16];
    time_t last_read;
    int poll_rate_ms;

    float cal_scale;
    float cal_offset;

    char formula[MAX_CONFIG_VALUE_LEN];
    int input_slots[8];
    int input_count;
} sensor_instance_t;

result_t sensor_instance_create_from_db(sensor_instance_t *instance, db_module_t *module, database_t *db);
result_t sensor_instance_read(sensor_instance_t *instance, float *value);
result_t sensor_instance_test(sensor_instance_t *instance);
void sensor_instance_destroy(sensor_instance_t *instance);

#endif
