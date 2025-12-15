#include "sensor_instance.h"
#include "utils/logger.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

// Helper: Parse I2C address from string
static uint8_t parse_i2c_address(const char *addr_str) {
    unsigned int addr;
    sscanf(addr_str, "0x%x", &addr);
    return (uint8_t)addr;
}

// Helper: Parse SPI bus.device from string
static void parse_spi_device(const char *device_str, int *bus, int *device) {
    sscanf(device_str, "%d.%d", bus, device);
}

// Helper: Apply calibration to raw ADC value
static float apply_calibration(sensor_instance_t *instance, int32_t raw_value) {
    // Linear interpolation: eng = (raw - raw_min) / (raw_max - raw_min) * (eng_max - eng_min) + eng_min
    float normalized = (float)(raw_value - instance->raw_min) / 
                      (float)(instance->raw_max - instance->raw_min);
    
    float eng_value = normalized * (instance->eng_max - instance->eng_min) + instance->eng_min;
    
    // Apply offset and scale
    eng_value = (eng_value + instance->offset) * instance->scale_factor;
    
    return eng_value;
}

// Helper: Apply moving average filter
static float apply_moving_average(sensor_instance_t *instance, float new_value) {
    if (!instance->enable_moving_avg || !instance->avg_buffer) {
        return new_value;
    }
    
    // Add new value to circular buffer
    instance->avg_buffer[instance->avg_index] = new_value;
    instance->avg_index = (instance->avg_index + 1) % instance->moving_avg_samples;
    
    // Calculate average
    float sum = 0.0f;
    for (int i = 0; i < instance->moving_avg_samples; i++) {
        sum += instance->avg_buffer[i];
    }
    
    return sum / instance->moving_avg_samples;
}

result_t sensor_instance_create_from_db(sensor_instance_t *instance, 
                                        db_module_t *module,
                                        database_t *db) {
    memset(instance, 0, sizeof(*instance));
    
    instance->module_id = module->id;
    instance->slot = module->slot;
    SAFE_STRNCPY(instance->name, module->name, sizeof(instance->name));
    
    pthread_mutex_init(&instance->mutex, NULL);
    
    result_t result = RESULT_OK;
    
    // Load sensor based on type
    if (strcmp(module->module_type, "physical") == 0) {
        instance->type = SENSOR_INSTANCE_PHYSICAL;
        
        db_physical_sensor_t sensor;
        if (db_physical_sensor_get(db, module->id, &sensor) != RESULT_OK) {
            LOG_ERROR("Failed to load physical sensor for module %d", module->id);
            return RESULT_ERROR;
        }
        
        instance->poll_rate_ms = sensor.poll_rate_ms;
        instance->timeout_ms = sensor.timeout_ms;
        
        // Initialize driver based on hardware type
        if (strcmp(sensor.sensor_type, "DS18B20") == 0) {
            result = ds18b20_init(&instance->driver.ds18b20, sensor.address);
        } else if (strcmp(sensor.sensor_type, "DHT22") == 0 || 
                   strcmp(sensor.sensor_type, "DHT11") == 0) {
            int gpio_pin = atoi(sensor.address);
            result = dht22_init(&instance->driver.dht22, gpio_pin);
        } else {
            LOG_WARNING("Unknown physical sensor type: %s", sensor.sensor_type);
            result = RESULT_ERROR;
        }
        
    } else if (strcmp(module->module_type, "adc") == 0) {
        instance->type = SENSOR_INSTANCE_ADC;
        
        db_adc_sensor_t sensor;
        if (db_adc_sensor_get(db, module->id, &sensor) != RESULT_OK) {
            LOG_ERROR("Failed to load ADC sensor for module %d", module->id);
            return RESULT_ERROR;
        }
        
        instance->poll_rate_ms = sensor.poll_rate_ms;
        instance->raw_min = sensor.raw_min;
        instance->raw_max = sensor.raw_max;
        instance->eng_min = sensor.eng_min;
        instance->eng_max = sensor.eng_max;
        instance->offset = sensor.offset;
        instance->scale_factor = sensor.scale_factor;
        instance->enable_moving_avg = sensor.enable_moving_avg;
        instance->moving_avg_samples = sensor.moving_avg_samples;
        
        // Allocate moving average buffer
        if (instance->enable_moving_avg) {
            instance->avg_buffer = calloc(instance->moving_avg_samples, sizeof(float));
        }
        
        // Initialize driver based on hardware
        if (strcmp(sensor.adc_hardware, "ADS1115") == 0 || 
            strcmp(sensor.adc_hardware, "ADS1015") == 0) {
            
            if (strcmp(sensor.interface, "i2c") == 0) {
                int bus = atoi(sensor.address);  // Simplified - should parse properly
                uint8_t addr = parse_i2c_address(sensor.address);
                
                result = ads1115_init(&instance->driver.ads1115, bus, addr);
                
                if (result == RESULT_OK) {
                    // Set gain
                    ads1115_gain_t gain = ADS1115_GAIN_4096MV;
                    int gain_val = atoi(sensor.gain);
                    switch (gain_val) {
                        case 6144: gain = ADS1115_GAIN_6144MV; break;
                        case 4096: gain = ADS1115_GAIN_4096MV; break;
                        case 2048: gain = ADS1115_GAIN_2048MV; break;
                        case 1024: gain = ADS1115_GAIN_1024MV; break;
                        case 512:  gain = ADS1115_GAIN_0512MV; break;
                        case 256:  gain = ADS1115_GAIN_0256MV; break;
                    }
                    ads1115_set_gain(&instance->driver.ads1115, gain);
                    
                    // Set sample rate
                    ads1115_sps_t sps = ADS1115_SPS_128;
                    switch (sensor.sample_rate_sps) {
                        case 8:   sps = ADS1115_SPS_8; break;
                        case 16:  sps = ADS1115_SPS_16; break;
                        case 32:  sps = ADS1115_SPS_32; break;
                        case 64:  sps = ADS1115_SPS_64; break;
                        case 128: sps = ADS1115_SPS_128; break;
                        case 250: sps = ADS1115_SPS_250; break;
                        case 475: sps = ADS1115_SPS_475; break;
                        case 860: sps = ADS1115_SPS_860; break;
                    }
                    ads1115_set_sample_rate(&instance->driver.ads1115, sps);
                }
            }
            
        } else if (strcmp(sensor.adc_hardware, "MCP3008") == 0) {
            int bus, device;
            parse_spi_device(sensor.address, &bus, &device);
            result = mcp3008_init(&instance->driver.mcp3008, bus, device, 3.3f);
        }
        
    } else if (strcmp(module->module_type, "web_poll") == 0) {
        instance->type = SENSOR_INSTANCE_WEB_POLL;
        
        db_web_poll_sensor_t sensor;
        if (db_web_poll_sensor_get(db, module->id, &sensor) != RESULT_OK) {
            LOG_ERROR("Failed to load web poll sensor for module %d", module->id);
            return RESULT_ERROR;
        }
        
        instance->poll_rate_ms = sensor.poll_interval_ms;
        instance->timeout_ms = sensor.timeout_ms;
        instance->scale_factor = sensor.scale_factor;
        
        result = web_poll_init(&instance->driver.web_poll, sensor.url, sensor.method);
        
        if (result == RESULT_OK) {
            web_poll_set_headers(&instance->driver.web_poll, sensor.headers);
            web_poll_set_json_path(&instance->driver.web_poll, sensor.json_path);
            instance->driver.web_poll.cache_on_error = sensor.cache_on_error;
        }
        
    } else if (strcmp(module->module_type, "static") == 0) {
        instance->type = SENSOR_INSTANCE_STATIC;
        
        db_static_sensor_t sensor;
        if (db_static_sensor_get(db, module->id, &sensor) != RESULT_OK) {
            LOG_ERROR("Failed to load static sensor for module %d", module->id);
            return RESULT_ERROR;
        }
        
        instance->current_value = sensor.value;
        instance->connected = true;
        
    } else if (strcmp(module->module_type, "calculated") == 0) {
        instance->type = SENSOR_INSTANCE_CALCULATED;
        // Calculated sensors are handled differently in sensor_manager
        instance->connected = true;
    }
    
    if (result == RESULT_OK) {
        instance->connected = true;
        LOG_INFO("Created sensor instance: slot=%d, type=%s", 
                instance->slot, module->module_type);
    } else {
        instance->connected = false;
        LOG_ERROR("Failed to create sensor instance: slot=%d", instance->slot);
    }
    
    return result;
}

void sensor_instance_destroy(sensor_instance_t *instance) {
    pthread_mutex_lock(&instance->mutex);
    
    switch (instance->type) {
        case SENSOR_INSTANCE_PHYSICAL:
            // Destroy based on actual driver type (would need to track this)
            break;
            
        case SENSOR_INSTANCE_ADC:
            // Destroy ADC driver
            break;
            
        case SENSOR_INSTANCE_WEB_POLL:
            web_poll_destroy(&instance->driver.web_poll);
            break;
            
        default:
            break;
    }
    
    if (instance->avg_buffer) {
        free(instance->avg_buffer);
        instance->avg_buffer = NULL;
    }
    
    pthread_mutex_unlock(&instance->mutex);
    pthread_mutex_destroy(&instance->mutex);
}

result_t sensor_instance_read(sensor_instance_t *instance, float *value) {
    pthread_mutex_lock(&instance->mutex);
    
    result_t result = RESULT_OK;
    float raw_value = 0.0f;
    
    switch (instance->type) {
        case SENSOR_INSTANCE_PHYSICAL:
            // Read based on driver type
            result = ds18b20_read_temperature(&instance->driver.ds18b20, &raw_value);
            break;
            
        case SENSOR_INSTANCE_ADC: {
            int16_t adc_raw;
            result = ads1115_read_channel(&instance->driver.ads1115, 0, &adc_raw);
            
            if (result == RESULT_OK) {
                instance->current_raw_value = adc_raw;
                raw_value = apply_calibration(instance, adc_raw);
            }
            break;
        }
            
        case SENSOR_INSTANCE_WEB_POLL:
            result = web_poll_fetch(&instance->driver.web_poll, &raw_value);
            if (result == RESULT_OK) {
                raw_value *= instance->scale_factor;
            }
            break;
            
        case SENSOR_INSTANCE_STATIC:
            raw_value = instance->current_value;
            result = RESULT_OK;
            break;
            
        case SENSOR_INSTANCE_CALCULATED:
            // Handled by sensor_manager
            raw_value = instance->current_value;
            result = RESULT_OK;
            break;
    }
    
    if (result == RESULT_OK) {
        // Apply filtering
        raw_value = apply_moving_average(instance, raw_value);
        
        instance->current_value = raw_value;
        instance->last_read = time(NULL);
        instance->consecutive_successes++;
        instance->consecutive_failures = 0;
        instance->connected = true;
        
        *value = raw_value;
    } else {
        instance->consecutive_failures++;
        instance->consecutive_successes = 0;
        
        if (instance->consecutive_failures >= 3) {
            instance->connected = false;
        }
    }
    
    pthread_mutex_unlock(&instance->mutex);
    
    return result;
}

result_t sensor_instance_test(sensor_instance_t *instance) {
    float value;
    result_t result = sensor_instance_read(instance, &value);
    
    if (result == RESULT_OK) {
        LOG_INFO("Sensor test OK: slot=%d, value=%.2f", instance->slot, value);
    } else {
        LOG_ERROR("Sensor test FAILED: slot=%d", instance->slot);
    }
    
    return result;
}

// Simple formula evaluator for calculated sensors
result_t sensor_instance_evaluate_formula(const char *formula, 
                                         const float *input_values,
                                         int input_count,
                                         float *result) {
    // Very simple evaluator - supports basic arithmetic and slot references
    // Format: (slot1 + slot2) / 2
    // This is a simplified version - production code should use a proper parser
    
    char formula_copy[MAX_PATH_LEN];
    SAFE_STRNCPY(formula_copy, formula, sizeof(formula_copy));
    
    // Replace slot references with actual values
    for (int i = 0; i < input_count; i++) {
        char slot_ref[16];
        char value_str[32];
        SAFE_SNPRINTF(slot_ref, sizeof(slot_ref), "slot%d", i);
        SAFE_SNPRINTF(value_str, sizeof(value_str), "%.6f", input_values[i]);
        
        // Simple string replacement (not robust)
        char *pos = strstr(formula_copy, slot_ref);
        if (pos) {
            // This is a placeholder - real implementation needs proper parsing
            LOG_WARNING("Formula evaluation not fully implemented");
            *result = input_values[0];  // Return first value as fallback
            return RESULT_OK;
        }
    }
    
    // For now, just average the inputs if formula is "(slot1 + slot2) / 2" pattern
    if (strstr(formula, "+") && strstr(formula, "/")) {
        float sum = 0.0f;
        for (int i = 0; i < input_count; i++) {
            sum += input_values[i];
        }
        *result = sum / input_count;
        return RESULT_OK;
    }
    
    LOG_ERROR("Formula evaluation not implemented for: %s", formula);
    return RESULT_ERROR;
}