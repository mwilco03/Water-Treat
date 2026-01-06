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
#include <limits.h>

/* VERSION_STRING is defined by CMake from git tags */
#ifndef VERSION_STRING
#define VERSION_STRING "0.1.0-dev"
#endif
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
    SENSOR_TYPE_WEB_POLL, SENSOR_TYPE_CALCULATED, SENSOR_TYPE_STATIC
} sensor_type_t;

/**
 * Data Quality Indicators (OPC UA compatible)
 *
 * Every sensor reading carries quality metadata. Stale data is never
 * presented as current. These values are transmitted in PROFINET input
 * data as the quality byte following each sensor value.
 *
 * Per DEVELOPMENT_GUIDELINES.md Part 2.4
 */
typedef enum {
    QUALITY_GOOD          = 0x00,  /* Fresh, valid reading */
    QUALITY_UNCERTAIN     = 0x40,  /* May be stale or sensor degraded */
    QUALITY_BAD           = 0x80,  /* Sensor failure, invalid reading */
    QUALITY_NOT_CONNECTED = 0xC0,  /* No communication with sensor */
} data_quality_t;

/**
 * Sensor Reading Structure
 *
 * Every sensor read operation produces this structure containing the
 * value, quality assessment, timing information, and diagnostic data.
 *
 * Per DEVELOPMENT_GUIDELINES.md Part 2.3
 */
typedef struct {
    float value;                    /* Engineering units */
    data_quality_t quality;         /* GOOD, UNCERTAIN, BAD, NOT_CONNECTED */
    uint64_t timestamp_us;          /* Microseconds since epoch */
    uint32_t raw_value;             /* Raw ADC/register value */
    uint8_t consecutive_failures;   /* For degradation detection */
} sensor_reading_t;

/* Time utility - must be defined before determine_quality which uses it */
static inline uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * Determine quality from reading state and configuration
 *
 * @param reading       Current sensor reading
 * @param stale_timeout_ms    Maximum age before UNCERTAIN
 * @param failure_threshold   Failures before BAD
 * @param range_min          Minimum valid value
 * @param range_max          Maximum valid value
 * @return Computed quality indicator
 */
static inline data_quality_t determine_quality(
    const sensor_reading_t *reading,
    uint32_t stale_timeout_ms,
    uint8_t failure_threshold,
    float range_min,
    float range_max)
{
    if (!reading) return QUALITY_NOT_CONNECTED;

    /* Check consecutive failures */
    if (reading->consecutive_failures >= failure_threshold) {
        return QUALITY_BAD;
    }

    /* Check staleness */
    uint64_t now_us = get_time_ms() * 1000;
    uint64_t age_ms = (now_us - reading->timestamp_us) / 1000;
    if (age_ms > stale_timeout_ms) {
        return QUALITY_UNCERTAIN;
    }

    /* Check range */
    if (reading->value < range_min || reading->value > range_max) {
        return QUALITY_UNCERTAIN;
    }

    return QUALITY_GOOD;
}

/**
 * Convert quality to display string
 */
static inline const char* quality_to_string(data_quality_t quality) {
    switch (quality) {
        case QUALITY_GOOD:          return "GOOD";
        case QUALITY_UNCERTAIN:     return "UNCERTAIN";
        case QUALITY_BAD:           return "BAD";
        case QUALITY_NOT_CONNECTED: return "N/C";
        default:                    return "UNKNOWN";
    }
}

/* SAFE_STRNCPY: Safe string copy with guaranteed null termination.
 * Uses memcpy to avoid GCC -Wstringop-truncation false positives when
 * source and destination buffers have the same size. */
#define SAFE_STRNCPY(dst, src, size) do { \
    if ((src) != NULL) { \
        size_t _srclen = strlen(src); \
        size_t _copylen = (_srclen < (size) - 1) ? _srclen : (size) - 1; \
        memcpy((dst), (src), _copylen); \
        (dst)[_copylen] = '\0'; \
    } else { \
        (dst)[0] = '\0'; \
    } \
} while(0)

#define SAFE_SNPRINTF(dst, size, fmt, ...) snprintf((dst), (size), (fmt), ##__VA_ARGS__)
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* MIN/MAX macros - use (a) and (b) directly to avoid type issues */
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define CLAMP(val, min, max) (MIN(MAX((val), (min)), (max)))
#define UNUSED(x) ((void)(x))
#define CHECK_NULL(ptr) do { if ((ptr) == NULL) return RESULT_INVALID_PARAM; } while(0)
#define CHECK_RESULT(expr) do { result_t _r = (expr); if (_r != RESULT_OK) return _r; } while(0)
#define SAFE_FREE(ptr) do { if ((ptr) != NULL) { free(ptr); (ptr) = NULL; } } while(0)

/* ============================================================================
 * Safe Number Parsing (P3 operator request - prevent silent failures)
 * ============================================================================
 * These functions return RESULT_OK on success with validated output,
 * or RESULT_PARSE_ERROR with an unchanged default value on failure.
 * Unlike atoi()/atof(), they don't silently return 0 on invalid input.
 */

/**
 * Parse string to int with validation
 * @param str     String to parse (may be NULL)
 * @param value   Output value (unchanged on error)
 * @param min     Minimum valid value
 * @param max     Maximum valid value
 * @return RESULT_OK on success, RESULT_PARSE_ERROR on invalid input
 */
static inline result_t safe_parse_int(const char *str, int *value, int min, int max) {
    if (!str || !value || str[0] == '\0') return RESULT_PARSE_ERROR;

    char *endptr;
    errno = 0;
    long val = strtol(str, &endptr, 10);

    /* Check for conversion errors */
    if (errno != 0 || endptr == str || *endptr != '\0') {
        return RESULT_PARSE_ERROR;
    }

    /* Check range (including int overflow) */
    if (val < min || val > max || val < INT32_MIN || val > INT32_MAX) {
        return RESULT_OUT_OF_RANGE;
    }

    *value = (int)val;
    return RESULT_OK;
}

/**
 * Parse string to float with validation
 * @param str     String to parse (may be NULL)
 * @param value   Output value (unchanged on error)
 * @param min     Minimum valid value
 * @param max     Maximum valid value
 * @return RESULT_OK on success, RESULT_PARSE_ERROR on invalid input
 */
static inline result_t safe_parse_float(const char *str, float *value, float min, float max) {
    if (!str || !value || str[0] == '\0') return RESULT_PARSE_ERROR;

    char *endptr;
    errno = 0;
    double val = strtod(str, &endptr);

    /* Check for conversion errors */
    if (errno != 0 || endptr == str || *endptr != '\0') {
        return RESULT_PARSE_ERROR;
    }

    /* Check range */
    if (val < min || val > max) {
        return RESULT_OUT_OF_RANGE;
    }

    *value = (float)val;
    return RESULT_OK;
}

/* ============================================================================
 * Centralized Status Strings
 * ============================================================================
 * These constants define canonical status strings used throughout the codebase
 * for sensor readings, module status, alarm states, and display purposes.
 * Using constants prevents typos and enables future i18n if needed.
 */
#define STATUS_OK           "ok"
#define STATUS_ERROR        "error"
#define STATUS_WARNING      "warning"
#define STATUS_UNKNOWN      "unknown"
#define STATUS_INACTIVE     "inactive"
#define STATUS_ACTIVE       "active"
#define STATUS_CONNECTED    "connected"
#define STATUS_DISCONNECTED "disconnected"
#define STATUS_GOOD         "good"
#define STATUS_BAD          "bad"
#define STATUS_FAIL         "fail"

/**
 * Status type classification for UI color mapping
 */
typedef enum {
    STATUS_TYPE_OK = 0,     /* Green - normal operation */
    STATUS_TYPE_WARNING,    /* Yellow - attention needed */
    STATUS_TYPE_ERROR,      /* Red - failure condition */
    STATUS_TYPE_UNKNOWN     /* Gray - indeterminate state */
} status_type_t;

/**
 * Classify a status string into a type for UI display
 */
static inline status_type_t status_classify(const char *status) {
    if (!status || !status[0]) return STATUS_TYPE_UNKNOWN;

    /* OK states */
    if (strcmp(status, STATUS_OK) == 0 ||
        strcmp(status, STATUS_GOOD) == 0 ||
        strcmp(status, STATUS_CONNECTED) == 0 ||
        strcmp(status, STATUS_ACTIVE) == 0) {
        return STATUS_TYPE_OK;
    }

    /* Error states */
    if (strcmp(status, STATUS_ERROR) == 0 ||
        strcmp(status, STATUS_FAIL) == 0 ||
        strcmp(status, STATUS_BAD) == 0 ||
        strcmp(status, STATUS_DISCONNECTED) == 0) {
        return STATUS_TYPE_ERROR;
    }

    /* Warning states */
    if (strcmp(status, STATUS_WARNING) == 0) {
        return STATUS_TYPE_WARNING;
    }

    return STATUS_TYPE_UNKNOWN;
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
