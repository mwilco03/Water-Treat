#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include "common.h"
#include "sensor_instance.h"
#include "db/database.h"
#include <pthread.h>

// Forward declarations
typedef struct profinet_manager_t profinet_manager_t;

/* Maximum slot number for O(1) lookup (PROFINET slots typically 1-64) */
#define SENSOR_MAX_SLOT 64

typedef struct {
    database_t *db;
    profinet_manager_t *profinet_mgr;

    sensor_instance_t *instances[MAX_SENSOR_INSTANCES];
    int instance_count;

    /* O(1) slot lookup: slot_map[slot] -> instance pointer (NULL if no sensor at slot) */
    sensor_instance_t *slot_map[SENSOR_MAX_SLOT + 1];

    pthread_t worker_thread;
    pthread_mutex_t mutex;
    volatile bool running;

    uint64_t total_reads;
    uint64_t successful_reads;
    uint64_t failed_reads;
} sensor_manager_t;

result_t sensor_manager_init(sensor_manager_t *mgr, database_t *db, profinet_manager_t *profinet_mgr);
result_t sensor_manager_start(sensor_manager_t *mgr);
result_t sensor_manager_stop(sensor_manager_t *mgr);
void sensor_manager_destroy(sensor_manager_t *mgr);
result_t sensor_manager_reload_sensors(sensor_manager_t *mgr);
result_t sensor_manager_get_sensor_value(sensor_manager_t *mgr, int slot, float *value);
result_t sensor_manager_test_sensor(sensor_manager_t *mgr, int slot);

#endif
