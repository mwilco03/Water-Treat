/**
 * @file driver_ds18b20.c
 * @brief DS18B20 1-Wire temperature sensor driver
 */

#include "common.h"
#include "utils/logger.h"
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>

#define W1_DEVICES_PATH     "/sys/bus/w1/devices"
#define W1_SLAVE_FILE       "w1_slave"
#define DS18B20_FAMILY_CODE "28"

typedef struct {
    char device_id[20];
    char device_path[512];
    bool initialized;
    float last_temp;
    uint64_t last_read_time;
} ds18b20_t;

static result_t find_device(const char *device_id, char *path, size_t path_size) {
    DIR *dir = opendir(W1_DEVICES_PATH);
    if (!dir) {
        LOG_ERROR("Cannot open 1-Wire devices directory");
        return RESULT_IO_ERROR;
    }
    
    struct dirent *entry;
    bool found = false;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, DS18B20_FAMILY_CODE, 2) == 0) {
            if (device_id == NULL || strlen(device_id) == 0 || 
                strcmp(entry->d_name, device_id) == 0) {
                snprintf(path, path_size, "%s/%s/%s", 
                         W1_DEVICES_PATH, entry->d_name, W1_SLAVE_FILE);
                found = true;
                break;
            }
        }
    }
    
    closedir(dir);
    return found ? RESULT_OK : RESULT_NOT_FOUND;
}

result_t ds18b20_init(ds18b20_t *dev, const char *device_id) {
    CHECK_NULL(dev);
    
    memset(dev, 0, sizeof(*dev));
    
    if (device_id && strlen(device_id) > 0) {
        SAFE_STRNCPY(dev->device_id, device_id, sizeof(dev->device_id));
    }
    
    result_t r = find_device(dev->device_id, dev->device_path, sizeof(dev->device_path));
    if (r != RESULT_OK) {
        LOG_ERROR("DS18B20 not found: %s", device_id ? device_id : "any");
        return r;
    }
    
    // Verify we can read the device
    int fd = open(dev->device_path, O_RDONLY);
    if (fd < 0) {
        LOG_ERROR("Cannot open DS18B20 device file");
        return RESULT_IO_ERROR;
    }
    close(fd);
    
    dev->initialized = true;
    LOG_INFO("DS18B20 initialized: %s", dev->device_path);
    return RESULT_OK;
}

result_t ds18b20_read(ds18b20_t *dev, float *temperature) {
    CHECK_NULL(dev);
    CHECK_NULL(temperature);
    
    if (!dev->initialized) return RESULT_NOT_INITIALIZED;
    
    int fd = open(dev->device_path, O_RDONLY);
    if (fd < 0) {
        LOG_ERROR("Failed to open DS18B20");
        return RESULT_IO_ERROR;
    }
    
    char buf[256];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    
    if (len <= 0) {
        LOG_ERROR("Failed to read DS18B20");
        return RESULT_IO_ERROR;
    }
    
    buf[len] = '\0';
    
    // Check CRC
    if (strstr(buf, "YES") == NULL) {
        LOG_WARNING("DS18B20 CRC check failed");
        return RESULT_ERROR;
    }
    
    // Find temperature value
    char *temp_str = strstr(buf, "t=");
    if (temp_str == NULL) {
        LOG_ERROR("Temperature not found in DS18B20 output");
        return RESULT_PARSE_ERROR;
    }
    
    int temp_raw = atoi(temp_str + 2);
    *temperature = temp_raw / 1000.0f;
    
    dev->last_temp = *temperature;
    dev->last_read_time = get_time_ms();
    
    return RESULT_OK;
}

result_t ds18b20_list_devices(char ***device_ids, int *count) {
    CHECK_NULL(device_ids);
    CHECK_NULL(count);
    
    DIR *dir = opendir(W1_DEVICES_PATH);
    if (!dir) return RESULT_IO_ERROR;
    
    // Count devices first
    int num_devices = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, DS18B20_FAMILY_CODE, 2) == 0) {
            num_devices++;
        }
    }
    
    if (num_devices == 0) {
        closedir(dir);
        *device_ids = NULL;
        *count = 0;
        return RESULT_OK;
    }
    
    *device_ids = calloc(num_devices, sizeof(char *));
    if (!*device_ids) {
        closedir(dir);
        return RESULT_NO_MEMORY;
    }
    
    rewinddir(dir);
    int idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < num_devices) {
        if (strncmp(entry->d_name, DS18B20_FAMILY_CODE, 2) == 0) {
            (*device_ids)[idx] = strdup(entry->d_name);
            idx++;
        }
    }
    
    closedir(dir);
    *count = idx;
    return RESULT_OK;
}

void ds18b20_close(ds18b20_t *dev) {
    if (dev) {
        dev->initialized = false;
        LOG_DEBUG("DS18B20 closed");
    }
}

/* Driver interface wrapper */
typedef struct {
    ds18b20_t device;
    float scale;
    float offset;
    bool use_fahrenheit;
} ds18b20_instance_t;

result_t driver_ds18b20_init(void **handle, const char *device_id) {
    ds18b20_instance_t *inst = calloc(1, sizeof(ds18b20_instance_t));
    if (!inst) return RESULT_NO_MEMORY;
    
    result_t r = ds18b20_init(&inst->device, device_id);
    if (r != RESULT_OK) {
        free(inst);
        return r;
    }
    
    inst->scale = 1.0f;
    inst->offset = 0.0f;
    inst->use_fahrenheit = false;
    
    *handle = inst;
    return RESULT_OK;
}

result_t driver_ds18b20_read(void *handle, float *value) {
    CHECK_NULL(handle);
    CHECK_NULL(value);
    
    ds18b20_instance_t *inst = (ds18b20_instance_t *)handle;
    float temp_c;
    
    result_t r = ds18b20_read(&inst->device, &temp_c);
    if (r != RESULT_OK) return r;
    
    if (inst->use_fahrenheit) {
        *value = temp_c * 9.0f / 5.0f + 32.0f;
    } else {
        *value = temp_c;
    }
    
    *value = *value * inst->scale + inst->offset;
    return RESULT_OK;
}

result_t driver_ds18b20_set_calibration(void *handle, float scale, float offset) {
    CHECK_NULL(handle);
    ds18b20_instance_t *inst = (ds18b20_instance_t *)handle;
    inst->scale = scale;
    inst->offset = offset;
    return RESULT_OK;
}

result_t driver_ds18b20_set_fahrenheit(void *handle, bool fahrenheit) {
    CHECK_NULL(handle);
    ds18b20_instance_t *inst = (ds18b20_instance_t *)handle;
    inst->use_fahrenheit = fahrenheit;
    return RESULT_OK;
}

void driver_ds18b20_close(void *handle) {
    if (handle) {
        ds18b20_instance_t *inst = (ds18b20_instance_t *)handle;
        ds18b20_close(&inst->device);
        free(inst);
    }
}
