/**
 * @file sensor_instance.c
 * @brief Sensor instance management
 */

#include "sensor_instance.h"
#include "utils/logger.h"
#include "drivers/driver_ds18b20.h"
#include "drivers/driver_dht22.h"
#include "drivers/driver_ads1115.h"
#include "drivers/driver_mcp3008.h"
#include "drivers/driver_web_poll.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <sys/time.h>

/* Helper: Get current time in microseconds */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* Helper: Update quality based on instance state */
static void update_quality(sensor_instance_t *instance) {
    if (!instance->connected) {
        instance->quality = QUALITY_NOT_CONNECTED;
        return;
    }

    if (instance->consecutive_failures >= instance->failure_threshold) {
        instance->quality = QUALITY_BAD;
        return;
    }

    /* Check staleness */
    uint64_t now_us = get_time_us();
    uint64_t age_ms = (now_us - instance->timestamp_us) / 1000;
    if (instance->timestamp_us > 0 && age_ms > instance->stale_timeout_ms) {
        instance->quality = QUALITY_UNCERTAIN;
        return;
    }

    /* Check range */
    if (instance->current_value < instance->range_min ||
        instance->current_value > instance->range_max) {
        instance->quality = QUALITY_UNCERTAIN;
        return;
    }

    instance->quality = QUALITY_GOOD;
}

// Helper: Parse SPI bus.device from string
static void parse_spi_device(const char *device_str, int *bus, int *device) {
    *bus = 0;
    *device = 0;
    if (device_str && strlen(device_str) > 0) {
        sscanf(device_str, "%d.%d", bus, device);
    }
}

// Helper: Apply calibration to raw ADC value
static float apply_calibration(sensor_instance_t *instance, int32_t raw_value) {
    // Avoid division by zero
    if (instance->raw_max == instance->raw_min) {
        return (float)raw_value * instance->scale_factor + instance->offset;
    }

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
    if (!instance->enable_moving_avg || !instance->avg_buffer || instance->moving_avg_samples <= 0) {
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
    instance->scale_factor = 1.0f;  // Default scale

    /* Initialize quality tracking with defaults per DEVELOPMENT_GUIDELINES.md */
    instance->quality = QUALITY_NOT_CONNECTED;
    instance->timestamp_us = 0;
    instance->stale_timeout_ms = 5000;   /* 5 seconds default */
    instance->failure_threshold = 3;     /* 3 failures before BAD */
    instance->range_min = -FLT_MAX;      /* No range check by default */
    instance->range_max = FLT_MAX;

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
            instance->driver_type = PHYSICAL_DRIVER_DS18B20;
            result = driver_ds18b20_init(&instance->driver_handle, sensor.address);
        } else if (strcmp(sensor.sensor_type, "DHT22") == 0 ||
                   strcmp(sensor.sensor_type, "DHT11") == 0) {
            instance->driver_type = PHYSICAL_DRIVER_DHT22;
            int gpio_pin = atoi(sensor.address);
            result = driver_dht22_init(&instance->driver_handle, gpio_pin, false);
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

        // Initialize driver based on hardware (use adc_type field)
        if (strcmp(sensor.adc_type, "ADS1115") == 0 ||
            strcmp(sensor.adc_type, "ADS1015") == 0) {
            instance->driver_type = ADC_DRIVER_ADS1115;
            if (strcmp(sensor.interface, "i2c") == 0) {
                result = driver_ads1115_init(&instance->driver_handle, sensor.address,
                                            sensor.bus, sensor.channel, sensor.gain);
            }
        } else if (strcmp(sensor.adc_type, "MCP3008") == 0) {
            instance->driver_type = ADC_DRIVER_MCP3008;
            int spi_bus, spi_device;
            parse_spi_device(sensor.address, &spi_bus, &spi_device);
            result = driver_mcp3008_init(&instance->driver_handle, spi_bus, spi_device,
                                         sensor.channel, sensor.reference_voltage);
        }

    } else if (strcmp(module->module_type, "web_poll") == 0) {
        instance->type = SENSOR_INSTANCE_WEB_POLL;

        db_web_poll_sensor_t sensor;
        if (db_web_poll_sensor_get(db, module->id, &sensor) != RESULT_OK) {
            LOG_ERROR("Failed to load web poll sensor for module %d", module->id);
            return RESULT_ERROR;
        }

        instance->poll_rate_ms = sensor.poll_rate_ms;
        instance->timeout_ms = sensor.timeout_ms;

        result = web_poll_init(&instance->driver.web_poll, sensor.url, sensor.method);

        if (result == RESULT_OK) {
            web_poll_set_headers(&instance->driver.web_poll, sensor.headers);
            web_poll_set_json_path(&instance->driver.web_poll, sensor.json_path);
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

        db_calculated_sensor_t sensor;
        if (db_calculated_sensor_get(db, module->id, &sensor) != RESULT_OK) {
            LOG_ERROR("Failed to load calculated sensor for module %d", module->id);
            return RESULT_ERROR;
        }

        SAFE_STRNCPY(instance->formula, sensor.formula, sizeof(instance->formula));
        instance->poll_rate_ms = sensor.update_rate_ms;

        // Parse input_sensors to get slot references (format: "slot1,slot2,slot3")
        instance->input_count = 0;
        char input_copy[256];
        SAFE_STRNCPY(input_copy, sensor.input_sensors, sizeof(input_copy));

        char *saveptr;
        char *token = strtok_r(input_copy, ",", &saveptr);
        while (token && instance->input_count < 8) {
            // Parse "slotN" format
            int slot = 0;
            if (sscanf(token, "slot%d", &slot) == 1 || sscanf(token, "%d", &slot) == 1) {
                instance->input_slots[instance->input_count++] = slot;
            }
            token = strtok_r(NULL, ",", &saveptr);
        }

        // Initialize formula evaluator with variable names
        if (instance->input_count > 0) {
            const char *var_names[8];
            char var_name_storage[8][16];
            for (int i = 0; i < instance->input_count; i++) {
                snprintf(var_name_storage[i], sizeof(var_name_storage[i]), "x%d", i);
                var_names[i] = var_name_storage[i];
            }

            result = formula_evaluator_init(&instance->formula_eval, sensor.formula,
                                           var_names, instance->input_count);
            if (result != RESULT_OK) {
                LOG_ERROR("Failed to compile formula for calculated sensor: %s", sensor.formula);
            }
        }

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

    // Clean up driver based on specific driver type
    if (instance->driver_handle) {
        switch (instance->driver_type) {
            case PHYSICAL_DRIVER_DS18B20:
                driver_ds18b20_close(instance->driver_handle);
                break;
            case PHYSICAL_DRIVER_DHT22:
                driver_dht22_close(instance->driver_handle);
                break;
            case ADC_DRIVER_ADS1115:
                driver_ads1115_close(instance->driver_handle);
                break;
            case ADC_DRIVER_MCP3008:
                driver_mcp3008_close(instance->driver_handle);
                break;
            default:
                break;
        }
        instance->driver_handle = NULL;
    }

    // Clean up web_poll context separately
    if (instance->type == SENSOR_INSTANCE_WEB_POLL) {
        web_poll_destroy(&instance->driver.web_poll);
    }

    // Clean up formula evaluator for calculated sensors
    if (instance->type == SENSOR_INSTANCE_CALCULATED) {
        formula_evaluator_destroy(&instance->formula_eval);
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
            if (instance->driver_handle) {
                result = driver_ds18b20_read(instance->driver_handle, &raw_value);
            } else {
                result = RESULT_NOT_INITIALIZED;
            }
            break;

        case SENSOR_INSTANCE_ADC:
            if (instance->driver_handle) {
                result = driver_ads1115_read(instance->driver_handle, &raw_value);
                if (result == RESULT_OK) {
                    instance->current_raw_value = (int32_t)(raw_value * 1000);  // Store as mV
                    raw_value = apply_calibration(instance, instance->current_raw_value);
                }
            } else {
                result = RESULT_NOT_INITIALIZED;
            }
            break;

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
        instance->last_read_ms = get_time_ms();
        instance->timestamp_us = get_time_us();  /* Quality tracking timestamp */
        instance->consecutive_successes++;
        instance->consecutive_failures = 0;
        instance->connected = true;

        *value = raw_value;
    } else {
        instance->consecutive_failures++;
        instance->consecutive_successes = 0;

        if (instance->consecutive_failures >= instance->failure_threshold) {
            instance->connected = false;
        }
    }

    /* Update quality indicator per DEVELOPMENT_GUIDELINES.md Part 2.4 */
    update_quality(instance);

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

/**
 * @brief Evaluate a formula for calculated sensors
 *
 * Uses TinyExpr library when available for full expression support,
 * otherwise falls back to simple pattern matching.
 */
result_t sensor_instance_evaluate_formula(const char *formula,
                                         const float *input_values,
                                         int input_count,
                                         float *result) {
    if (!formula || !input_values || !result || input_count <= 0) {
        return RESULT_INVALID_PARAM;
    }

    // Create temporary evaluator for this formula
    formula_evaluator_t eval;
    const char *var_names[8];
    char var_name_storage[8][16];

    for (int i = 0; i < input_count && i < 8; i++) {
        snprintf(var_name_storage[i], sizeof(var_name_storage[i]), "x%d", i);
        var_names[i] = var_name_storage[i];
    }

    result_t res = formula_evaluator_init(&eval, formula, var_names, input_count);
    if (res != RESULT_OK) {
        LOG_WARNING("Failed to compile formula: %s", formula);
        // Fallback: return average
        float sum = 0.0f;
        for (int i = 0; i < input_count; i++) {
            sum += input_values[i];
        }
        *result = sum / input_count;
        return RESULT_OK;
    }

    res = formula_evaluator_evaluate(&eval, input_values, result);
    formula_evaluator_destroy(&eval);

    return res;
}

/**
 * @brief Evaluate a calculated sensor using its pre-compiled formula
 */
result_t sensor_instance_evaluate_calculated(sensor_instance_t *instance,
                                             const float *input_values,
                                             float *result) {
    if (!instance || instance->type != SENSOR_INSTANCE_CALCULATED) {
        return RESULT_INVALID_PARAM;
    }

    return formula_evaluator_evaluate(&instance->formula_eval, input_values, result);
}

/**
 * @brief Read sensor with full quality information
 *
 * Per DEVELOPMENT_GUIDELINES.md Part 2.3, this produces the complete
 * sensor_reading_t structure with value, quality, timestamp, and raw value.
 */
result_t sensor_instance_read_with_quality(sensor_instance_t *instance, sensor_reading_t *reading) {
    CHECK_NULL(instance);
    CHECK_NULL(reading);

    float value;
    result_t result = sensor_instance_read(instance, &value);

    pthread_mutex_lock(&instance->mutex);

    reading->value = instance->current_value;
    reading->quality = instance->quality;
    reading->timestamp_us = instance->timestamp_us;
    reading->raw_value = (uint32_t)instance->current_raw_value;
    reading->consecutive_failures = (uint8_t)instance->consecutive_failures;

    pthread_mutex_unlock(&instance->mutex);

    return result;
}

/**
 * @brief Get current quality indicator for sensor
 *
 * Thread-safe accessor for quality without performing a new read.
 */
data_quality_t sensor_instance_get_quality(sensor_instance_t *instance) {
    if (!instance) return QUALITY_NOT_CONNECTED;

    pthread_mutex_lock(&instance->mutex);
    data_quality_t q = instance->quality;
    pthread_mutex_unlock(&instance->mutex);

    return q;
}
