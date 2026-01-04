#include "sensor_manager.h"
#include "profinet/profinet_manager.h"
#include "alarms/alarm_manager.h"
#include "db/db_modules.h"
#include "db/db_events.h"
#include "utils/logger.h"

#ifdef LED_SUPPORT
#include "hal/led_status.h"
extern led_status_manager_t g_led_mgr;
#endif

#include <unistd.h>
#include <string.h>

/* Data collected from sensor reads - used to process outside mutex */
typedef struct {
    int module_id;
    int slot;
    float value;
    bool success;
    data_quality_t quality;
    float last_value;  /* For failed reads */
} sensor_read_result_t;

#define MAX_SENSOR_UPDATES 64

// Worker thread function
static void* sensor_worker_thread(void *arg) {
    sensor_manager_t *mgr = (sensor_manager_t *)arg;

    LOG_INFO("Sensor worker thread started");

    /* Pre-allocated buffer for sensor updates - avoids allocation in loop */
    sensor_read_result_t updates[MAX_SENSOR_UPDATES];
    int update_count = 0;

    while (mgr->running) {
        update_count = 0;

        /*
         * CRITICAL SECTION: Only hold mutex while reading sensors
         * LED updates and alarm checks moved outside to reduce contention
         */
        pthread_mutex_lock(&mgr->mutex);

        // Read all sensors - collect results for processing outside mutex
        for (int i = 0; i < mgr->instance_count && update_count < MAX_SENSOR_UPDATES; i++) {
            sensor_instance_t *instance = mgr->instances[i];
            if (!instance) continue;

            // Check if it's time to poll this sensor
            uint64_t now_ms = get_time_ms();
            uint64_t elapsed_ms = now_ms - instance->last_read_ms;

            if (elapsed_ms >= (uint64_t)instance->poll_rate_ms) {
                sensor_read_result_t *upd = &updates[update_count];
                upd->module_id = instance->module_id;
                upd->slot = instance->slot;
                upd->last_value = instance->current_value;

                result_t result = sensor_instance_read(instance, &upd->value);
                upd->success = (result == RESULT_OK);
                upd->quality = sensor_instance_get_quality(instance);

                mgr->total_reads++;
                if (upd->success) {
                    mgr->successful_reads++;
                } else {
                    mgr->failed_reads++;
                }

                update_count++;
            }
        }

        pthread_mutex_unlock(&mgr->mutex);
        /* END CRITICAL SECTION */

        /*
         * Process collected sensor updates OUTSIDE mutex
         * This reduces lock contention with other threads accessing sensor_manager
         */
        for (int i = 0; i < update_count; i++) {
            sensor_read_result_t *upd = &updates[i];

            if (upd->success) {
                // Check alarm rules for this sensor value
                if (alarm_manager_is_running()) {
                    alarm_manager_check_value(upd->module_id, upd->value);
                }

#ifdef LED_SUPPORT
                // Update LED status for this sensor slot (slots 1-4 map to sensor LEDs 0-3)
                if (g_led_mgr.initialized && upd->slot >= 1 && upd->slot <= 4) {
                    bool has_alarm = false;
                    bool has_warning = false;
                    if (alarm_manager_is_running()) {
                        int critical_count = 0, high_count = 0;
                        alarm_manager_get_active_by_severity(ALARM_SEVERITY_CRITICAL, &critical_count);
                        alarm_manager_get_active_by_severity(ALARM_SEVERITY_HIGH, &high_count);
                        has_alarm = (critical_count > 0);
                        has_warning = (high_count > 0);
                    }
                    int led_index = upd->slot - 1;
                    led_set_sensor_status(&g_led_mgr, led_index, has_alarm, has_warning, false);
                }
#endif

                /*
                 * Write to PROFINET with quality (5-byte format)
                 * Per DEVELOPMENT_GUIDELINES.md Part 1.2:
                 *   Bytes 0-3: Float32 value (big-endian)
                 *   Byte 4:    Quality indicator
                 */
                if (mgr->profinet_mgr) {
                    profinet_manager_update_input_with_quality(
                        upd->slot,
                        0,  /* subslot */
                        upd->value,
                        upd->quality
                    );

                    /* Set IOPS based on quality */
                    uint8_t iops = (upd->quality == QUALITY_GOOD) ? PNET_IOXS_GOOD : PNET_IOXS_BAD;
                    profinet_manager_set_input_iops(mgr->profinet_mgr,
                                                   upd->slot, 0, iops);
                }

                LOG_DEBUG("Read sensor slot=%d: %.2f", upd->slot, upd->value);

            } else {
#ifdef LED_SUPPORT
                /* Set LED to fault status for failed sensor read */
                if (g_led_mgr.initialized && upd->slot >= 1 && upd->slot <= 4) {
                    int led_index = upd->slot - 1;
                    led_set_status(&g_led_mgr, LED_FUNC_SENSOR_1 + led_index, LED_STATUS_FAULT);
                }
#endif

                /*
                 * Send last known value with BAD quality on error
                 * Per DEVELOPMENT_GUIDELINES.md - stale data is marked, not hidden
                 */
                if (mgr->profinet_mgr) {
                    profinet_manager_update_input_with_quality(
                        upd->slot,
                        0,  /* subslot */
                        upd->last_value,  /* last known value */
                        upd->quality  /* will be BAD or NOT_CONNECTED */
                    );
                    profinet_manager_set_input_iops(mgr->profinet_mgr,
                                                   upd->slot, 0,
                                                   PNET_IOXS_BAD);
                }

                LOG_WARNING("Failed to read sensor slot=%d", upd->slot);
            }
        }

        // Sleep for a short interval (10ms)
        usleep(10000);
    }

    LOG_INFO("Sensor worker thread stopped");
    return NULL;
}

result_t sensor_manager_init(sensor_manager_t *mgr, database_t *db, 
                             profinet_manager_t *profinet_mgr) {
    memset(mgr, 0, sizeof(*mgr));
    
    mgr->db = db;
    mgr->profinet_mgr = profinet_mgr;
    
    pthread_mutex_init(&mgr->mutex, NULL);
    
    // Load sensors from database
    result_t result = sensor_manager_reload_sensors(mgr);
    if (result != RESULT_OK) {
        LOG_ERROR("Failed to load sensors from database");
        return result;
    }
    
    LOG_INFO("Sensor manager initialized with %d sensors", mgr->instance_count);
    return RESULT_OK;
}

result_t sensor_manager_start(sensor_manager_t *mgr) {
    mgr->running = true;
    
    if (pthread_create(&mgr->worker_thread, NULL, sensor_worker_thread, mgr) != 0) {
        LOG_ERROR("Failed to create sensor worker thread");
        mgr->running = false;
        return RESULT_ERROR;
    }
    
    LOG_INFO("Sensor manager started");
    return RESULT_OK;
}

result_t sensor_manager_stop(sensor_manager_t *mgr) {
    if (!mgr->running) {
        return RESULT_OK;
    }
    
    mgr->running = false;
    
    if (pthread_join(mgr->worker_thread, NULL) != 0) {
        LOG_ERROR("Failed to join sensor worker thread");
        return RESULT_ERROR;
    }
    
    LOG_INFO("Sensor manager stopped");
    return RESULT_OK;
}

void sensor_manager_destroy(sensor_manager_t *mgr) {
    pthread_mutex_lock(&mgr->mutex);
    
    // Destroy all sensor instances
    for (int i = 0; i < mgr->instance_count; i++) {
        if (mgr->instances[i]) {
            sensor_instance_destroy(mgr->instances[i]);
            free(mgr->instances[i]);
            mgr->instances[i] = NULL;
        }
    }
    
    mgr->instance_count = 0;
    
    pthread_mutex_unlock(&mgr->mutex);
    pthread_mutex_destroy(&mgr->mutex);
    
    LOG_INFO("Sensor manager destroyed");
}

result_t sensor_manager_reload_sensors(sensor_manager_t *mgr) {
    pthread_mutex_lock(&mgr->mutex);
    
    // Destroy existing instances and clear slot map
    for (int i = 0; i < mgr->instance_count; i++) {
        if (mgr->instances[i]) {
            sensor_instance_destroy(mgr->instances[i]);
            free(mgr->instances[i]);
            mgr->instances[i] = NULL;
        }
    }

    mgr->instance_count = 0;
    memset(mgr->slot_map, 0, sizeof(mgr->slot_map));
    
    // Load modules from database
    db_module_t *modules = NULL;
    int module_count = 0;
    
    result_t result = db_module_list(mgr->db, &modules, &module_count);
    if (result != RESULT_OK) {
        pthread_mutex_unlock(&mgr->mutex);
        return result;
    }
    
    // Create sensor instances
    for (int i = 0; i < module_count && mgr->instance_count < MAX_SENSOR_INSTANCES; i++) {
        db_module_t *module = &modules[i];
        
        // Skip disabled modules
        if (strcmp(module->status, "disabled") == 0) {
            continue;
        }
        
        sensor_instance_t *instance = calloc(1, sizeof(sensor_instance_t));
        if (!instance) {
            LOG_ERROR("Failed to allocate sensor instance");
            continue;
        }
        
        result = sensor_instance_create_from_db(instance, module, mgr->db);
        if (result == RESULT_OK) {
            mgr->instances[mgr->instance_count++] = instance;

            /* Update slot_map for O(1) lookup */
            if (instance->slot >= 0 && instance->slot <= SENSOR_MAX_SLOT) {
                mgr->slot_map[instance->slot] = instance;
            }

            // Add module to PROFINET if configured
            if (mgr->profinet_mgr && instance->type != SENSOR_INSTANCE_CALCULATED) {
                profinet_manager_add_module(
                    mgr->profinet_mgr,
                    instance->slot,
                    0x00000001,  // module_ident
                    0,           // subslot
                    0x00000001,  // submodule_ident
                    sizeof(float), // input_length
                    0            // output_length
                );
            }
        } else {
            free(instance);
            LOG_ERROR("Failed to create sensor instance for module %d", module->id);
        }
    }
    
    free(modules);
    
    pthread_mutex_unlock(&mgr->mutex);
    
    LOG_INFO("Reloaded %d sensors", mgr->instance_count);
    DB_EVENT_INFO(mgr->db, "sensor_manager", "Reloaded sensor configuration");
    
    return RESULT_OK;
}

result_t sensor_manager_get_sensor_value(sensor_manager_t *mgr, int slot, float *value) {
    pthread_mutex_lock(&mgr->mutex);

    /* O(1) lookup via slot_map */
    if (slot >= 0 && slot <= SENSOR_MAX_SLOT && mgr->slot_map[slot]) {
        *value = mgr->slot_map[slot]->current_value;
        pthread_mutex_unlock(&mgr->mutex);
        return RESULT_OK;
    }

    pthread_mutex_unlock(&mgr->mutex);
    return RESULT_NOT_FOUND;
}

result_t sensor_manager_test_sensor(sensor_manager_t *mgr, int slot) {
    pthread_mutex_lock(&mgr->mutex);

    /* O(1) lookup via slot_map */
    if (slot >= 0 && slot <= SENSOR_MAX_SLOT && mgr->slot_map[slot]) {
        result_t result = sensor_instance_test(mgr->slot_map[slot]);
        pthread_mutex_unlock(&mgr->mutex);
        return result;
    }

    pthread_mutex_unlock(&mgr->mutex);
    return RESULT_NOT_FOUND;
}