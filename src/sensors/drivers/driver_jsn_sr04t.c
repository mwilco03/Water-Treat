#include "driver_jsn_sr04t.h"
#include "utils/logger.h"
#include <unistd.h>
#include <time.h>
#include <math.h>

#define JSN_SR04T_MIN_DISTANCE_CM  20.0f
#define JSN_SR04T_MAX_DISTANCE_CM  600.0f
#define JSN_SR04T_DEFAULT_TIMEOUT_US  38000  // 38ms for 600cm max range

// Calculate speed of sound based on temperature
static float calculate_speed_of_sound(float temperature) {
    // Speed of sound in air: v = 331.3 + 0.606 * T (m/s)
    return 331.3f + 0.606f * temperature;
}

result_t jsn_sr04t_init(jsn_sr04t_device_t *dev, int trigger_pin, int echo_pin) {
    memset(dev, 0, sizeof(*dev));
    
    // Export trigger pin (output)
    result_t result = gpio_export(&dev->trigger, trigger_pin);
    if (result != RESULT_OK) {
        LOG_ERROR("Failed to export JSN-SR04T trigger pin %d", trigger_pin);
        return result;
    }
    gpio_set_direction(&dev->trigger, GPIO_DIR_OUT);
    gpio_write(&dev->trigger, false);
    
    // Export echo pin (input)
    result = gpio_export(&dev->echo, echo_pin);
    if (result != RESULT_OK) {
        LOG_ERROR("Failed to export JSN-SR04T echo pin %d", echo_pin);
        gpio_unexport(&dev->trigger);
        return result;
    }
    gpio_set_direction(&dev->echo, GPIO_DIR_IN);
    
    dev->temperature = 20.0f;  // Default 20°C
    dev->timeout_us = JSN_SR04T_DEFAULT_TIMEOUT_US;
    dev->last_distance = 0.0f;
    dev->last_read = 0;
    
    LOG_INFO("Initialized JSN-SR04T on trigger=%d, echo=%d", trigger_pin, echo_pin);
    return RESULT_OK;
}

void jsn_sr04t_destroy(jsn_sr04t_device_t *dev) {
    gpio_unexport(&dev->trigger);
    gpio_unexport(&dev->echo);
}

result_t jsn_sr04t_set_temperature(jsn_sr04t_device_t *dev, float temperature) {
    dev->temperature = temperature;
    LOG_DEBUG("JSN-SR04T temperature set to %.1f°C", temperature);
    return RESULT_OK;
}

result_t jsn_sr04t_read_distance_cm(jsn_sr04t_device_t *dev, float *distance_cm) {
    struct timespec start_time, end_time;
    bool echo_state;
    
    // Ensure at least 60ms between measurements (sensor limitation)
    time_t now = time(NULL);
    if (now - dev->last_read < 1) {
        usleep(60000);  // Wait 60ms
    }
    
    // Send 10us trigger pulse
    gpio_write(&dev->trigger, true);
    usleep(10);
    gpio_write(&dev->trigger, false);
    
    // Wait for echo to go high (start of pulse)
    int timeout_counter = 0;
    int max_timeout = 10000;  // 10ms timeout for echo start
    
    while (timeout_counter < max_timeout) {
        gpio_read(&dev->echo, &echo_state);
        if (echo_state) {
            break;
        }
        usleep(1);
        timeout_counter++;
    }
    
    if (timeout_counter >= max_timeout) {
        LOG_WARNING("JSN-SR04T timeout waiting for echo start");
        return RESULT_TIMEOUT;
    }
    
    // Record start time
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    // Wait for echo to go low (end of pulse)
    timeout_counter = 0;
    max_timeout = dev->timeout_us;
    
    while (timeout_counter < max_timeout) {
        gpio_read(&dev->echo, &echo_state);
        if (!echo_state) {
            break;
        }
        usleep(1);
        timeout_counter++;
    }
    
    if (timeout_counter >= max_timeout) {
        LOG_WARNING("JSN-SR04T timeout waiting for echo end");
        return RESULT_TIMEOUT;
    }
    
    // Record end time
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    
    // Calculate pulse duration in microseconds
    long duration_us = (end_time.tv_sec - start_time.tv_sec) * 1000000L +
                       (end_time.tv_nsec - start_time.tv_nsec) / 1000L;
    
    // Calculate distance with temperature compensation
    // Distance = (time * speed_of_sound) / 2
    // Speed of sound varies with temperature
    float speed_of_sound = calculate_speed_of_sound(dev->temperature);  // m/s
    float distance = (duration_us * speed_of_sound * 100.0f) / (2.0f * 1000000.0f);  // Convert to cm
    
    // Validate range
    if (distance < JSN_SR04T_MIN_DISTANCE_CM || distance > JSN_SR04T_MAX_DISTANCE_CM) {
        LOG_DEBUG("JSN-SR04T distance out of range: %.1fcm", distance);
        return RESULT_OUT_OF_RANGE;
    }
    
    dev->last_distance = distance;
    dev->last_read = now;
    
    *distance_cm = distance;
    
    LOG_DEBUG("JSN-SR04T measured: %.1fcm (pulse: %ldus, temp: %.1f°C)", 
              distance, duration_us, dev->temperature);
    
    return RESULT_OK;
}

result_t jsn_sr04t_read_distance_mm(jsn_sr04t_device_t *dev, float *distance_mm) {
    float distance_cm;
    result_t result = jsn_sr04t_read_distance_cm(dev, &distance_cm);
    
    if (result == RESULT_OK) {
        *distance_mm = distance_cm * 10.0f;
    }
    
    return result;
}

result_t jsn_sr04t_read_average(jsn_sr04t_device_t *dev, int samples, float *distance_cm) {
    if (samples < 1) {
        return RESULT_ERROR;
    }
    
    float sum = 0.0f;
    int valid_samples = 0;
    
    for (int i = 0; i < samples; i++) {
        float distance;
        result_t result = jsn_sr04t_read_distance_cm(dev, &distance);
        
        if (result == RESULT_OK) {
            sum += distance;
            valid_samples++;
        }
        
        // Wait between samples (sensor needs 60ms minimum)
        if (i < samples - 1) {
            usleep(60000);
        }
    }
    
    if (valid_samples == 0) {
        LOG_ERROR("JSN-SR04T: No valid samples out of %d attempts", samples);
        return RESULT_ERROR;
    }
    
    *distance_cm = sum / valid_samples;
    
    if (valid_samples < samples) {
        LOG_WARNING("JSN-SR04T: Only %d/%d samples valid", valid_samples, samples);
    }
    
    return RESULT_OK;
}