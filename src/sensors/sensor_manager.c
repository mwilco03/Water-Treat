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

// Worker thread function
static void* sensor_worker_thread(void *arg) {
    sensor_manager_t *mgr = (sensor_manager_t *)arg;
    
    LOG_INFO("Sensor worker thread started");
    
    while (mgr->running) {
        struct timespec start_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        
        pthread_mutex_lock(&mgr->mutex);
        
        // Read all sensors
        for (int i = 0; i < mgr->instance_count; i++) {
            sensor_instance_t *instance = mgr->instances[i];
            if (!instance) continue;
            
            // Check if it's time to poll this sensor
            uint64_t now_ms = get_time_ms();
            uint64_t elapsed_ms = now_ms - instance->last_read_ms;
            
            if (elapsed_ms >= (uint64_t)instance->poll_rate_ms) {
                float value;
                result_t result = sensor_instance_read(instance, &value);
                
                mgr->total_reads++;
                
                if (result == RESULT_OK) {
                    mgr->successful_reads++;

                    // Check alarm rules for this sensor value
                    if (alarm_manager_is_running()) {
                        alarm_manager_check_value(instance->module_id, value);
                    }

#ifdef LED_SUPPORT
                    // Update LED status for this sensor slot (slots 1-4 map to sensor LEDs 0-3)
                    if (g_led_mgr.initialized && instance->slot >= 1 && instance->slot <= 4) {
                        bool has_alarm = false;
                        bool has_warning = false;
                        if (alarm_manager_is_running()) {
                            int critical_count = 0, high_count = 0;
                            alarm_manager_get_active_by_severity(ALARM_SEVERITY_CRITICAL, &critical_count);
                            alarm_manager_get_active_by_severity(ALARM_SEVERITY_HIGH, &high_count);
                            has_alarm = (critical_count > 0);
                            has_warning = (high_count > 0);
                        }
                        int led_index = instance->slot - 1;
                        led_set_sensor_status(&g_led_mgr, led_index, has_alarm, has_warning, false);
                    }
#endif

                    // Write to PROFINET
                    if (mgr->profinet_mgr) {
                        uint8_t data[4];

                        // Convert float to bytes (little-endian)
                        memcpy(data, &value, sizeof(float));

                        profinet_manager_write_input_data(
                            mgr->profinet_mgr,
                            instance->slot,
                            0,  // subslot
                            data,
                            sizeof(float)
                        );

                        // Set IOPS to GOOD
                        profinet_manager_set_input_iops(mgr->profinet_mgr,
                                                       instance->slot, 0,
                                                       PNET_IOXS_GOOD);
                    }

                    LOG_DEBUG("Read sensor slot=%d: %.2f", instance->slot, value);

                } else {
                    mgr->failed_reads++;

#ifdef LED_SUPPORT
                    // Set LED to fault status for failed sensor read
                    if (g_led_mgr.initialized && instance->slot >= 1 && instance->slot <= 4) {
                        int led_index = instance->slot - 1;
                        led_set_status(&g_led_mgr, LED_FUNC_SENSOR_1 + led_index, LED_STATUS_FAULT);
                    }
#endif

                    // Set IOPS to BAD on error
                    if (mgr->profinet_mgr) {
                        profinet_manager_set_input_iops(mgr->profinet_mgr,
                                                       instance->slot, 0,
                                                       PNET_IOXS_BAD);
                    }

                    LOG_WARNING("Failed to read sensor slot=%d", instance->slot);
                }
            }
        }
        
        pthread_mutex_unlock(&mgr->mutex);
        
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
    
    // Destroy existing instances
    for (int i = 0; i < mgr->instance_count; i++) {
        if (mgr->instances[i]) {
            sensor_instance_destroy(mgr->instances[i]);
            free(mgr->instances[i]);
            mgr->instances[i] = NULL;
        }
    }
    
    mgr->instance_count = 0;
    
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
    
    for (int i = 0; i < mgr->instance_count; i++) {
        if (mgr->instances[i] && mgr->instances[i]->slot == slot) {
            *value = mgr->instances[i]->current_value;
            pthread_mutex_unlock(&mgr->mutex);
            return RESULT_OK;
        }
    }
    
    pthread_mutex_unlock(&mgr->mutex);
    return RESULT_NOT_FOUND;
}

result_t sensor_manager_test_sensor(sensor_manager_t *mgr, int slot) {
    pthread_mutex_lock(&mgr->mutex);
    
    for (int i = 0; i < mgr->instance_count; i++) {
        if (mgr->instances[i] && mgr->instances[i]->slot == slot) {
            result_t result = sensor_instance_test(mgr->instances[i]);
            pthread_mutex_unlock(&mgr->mutex);
            return result;
        }
    }
    
    pthread_mutex_unlock(&mgr->mutex);
    return RESULT_NOT_FOUND;
}