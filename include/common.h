#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define VERSION_STRING "1.0.0"
#define MAX_NAME_LEN 64
#define MAX_PATH_LEN 256
#define MAX_CONFIG_VALUE_LEN 512
#define MAX_MODULES 64
#define MAX_SENSOR_INSTANCES 64
#define MAX_DATA_SIZE 256

typedef enum {
    RESULT_OK = 0, RESULT_ERROR = -1, RESULT_TIMEOUT = -2, RESULT_NOT_FOUND = -3,
    RESULT_INVALID_PARAM = -4, RESULT_NO_MEMORY = -5, RESULT_BUSY = -6,
    RESULT_NOT_SUPPORTED = -7, RESULT_IO_ERROR = -8, RESULT_PARSE_ERROR = -9,
    RESULT_OUT_OF_RANGE = -10, RESULT_ALREADY_EXISTS = -11, RESULT_NOT_INITIALIZED = -12
} result_t;

typedef enum {
    LOG_LEVEL_TRACE = 0, LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR, LOG_LEVEL_FATAL, LOG_LEVEL_NONE
} log_level_t;

typedef enum {
    CONNECTION_STATE_DISCONNECTED = 0, CONNECTION_STATE_CONNECTING,
    CONNECTION_STATE_CONNECTED, CONNECTION_STATE_ERROR
} connection_state_t;

typedef enum {
    SENSOR_TYPE_UNKNOWN = 0, SENSOR_TYPE_PHYSICAL, SENSOR_TYPE_ADC,
    SENSOR_TYPE_WEB_POLL, SENSOR_TYPE_CALCULATED, SENSOR_TYPE_STATIC, SENSOR_TYPE_MODBUS
} sensor_type_t;

#define SAFE_STRNCPY(dst, src, size) do { \
    if ((src) != NULL) { strncpy((dst), (src), (size) - 1); (dst)[(size) - 1] = '\0'; } \
    else { (dst)[0] = '\0'; } \
} while(0)

#define SAFE_SNPRINTF(dst, size, fmt, ...) snprintf((dst), (size), (fmt), ##__VA_ARGS__)
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define CLAMP(val, min, max) (MIN(MAX((val), (min)), (max)))
#define UNUSED(x) ((void)(x))
#define CHECK_NULL(ptr) do { if ((ptr) == NULL) return RESULT_INVALID_PARAM; } while(0)
#define CHECK_RESULT(expr) do { result_t _r = (expr); if (_r != RESULT_OK) return _r; } while(0)
#define SAFE_FREE(ptr) do { if ((ptr) != NULL) { free(ptr); (ptr) = NULL; } } while(0)

static inline uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline const char* result_to_string(result_t result) {
    switch (result) {
        case RESULT_OK: return "OK";
        case RESULT_ERROR: return "Error";
        case RESULT_TIMEOUT: return "Timeout";
        case RESULT_NOT_FOUND: return "Not Found";
        default: return "Unknown";
    }
}

#endif
