/**
 * @file health_check.h
 * @brief Health check endpoint for monitoring
 *
 * Provides both file-based and HTTP health status for external monitoring
 * systems (Prometheus, Nagios, etc.)
 */

#ifndef HEALTH_CHECK_H
#define HEALTH_CHECK_H

#include "common.h"
#include "db/database.h"
#include "config/config.h"

/* Health status levels */
typedef enum {
    HEALTH_STATUS_OK = 0,        /* All systems operational */
    HEALTH_STATUS_DEGRADED = 1,  /* Partial functionality (e.g., PROFINET disconnected) */
    HEALTH_STATUS_CRITICAL = 2,  /* Critical failure */
    HEALTH_STATUS_UNKNOWN = 3    /* Status cannot be determined */
} health_status_t;

/* Individual subsystem health */
typedef struct {
    health_status_t status;
    char name[32];
    char message[128];
    uint64_t last_check_ms;
} subsystem_health_t;

/* Overall system health snapshot */
typedef struct {
    health_status_t overall_status;
    uint64_t uptime_seconds;
    uint64_t timestamp;

    /* Subsystem health */
    subsystem_health_t profinet;
    subsystem_health_t sensors;
    subsystem_health_t actuators;
    subsystem_health_t database;
    subsystem_health_t alarms;

    /* Metrics */
    int active_sensors;
    int failed_sensors;
    int active_alarms;
    int active_actuators;
    float cpu_usage_percent;
    float memory_usage_percent;
} health_snapshot_t;

/* health_config_t is defined in config/config.h */

/**
 * @brief Initialize the health check module
 * @param db Database connection
 * @param config Health check configuration
 * @return RESULT_OK on success
 */
result_t health_check_init(database_t *db, const health_config_t *config);

/**
 * @brief Start the health check background thread
 * @return RESULT_OK on success
 */
result_t health_check_start(void);

/**
 * @brief Stop the health check module
 */
void health_check_stop(void);

/**
 * @brief Shutdown and cleanup
 */
void health_check_shutdown(void);

/**
 * @brief Check if health check is running
 */
bool health_check_is_running(void);

/**
 * @brief Get current health snapshot (thread-safe)
 * @param snapshot Output snapshot
 * @return RESULT_OK on success
 */
result_t health_check_get_snapshot(health_snapshot_t *snapshot);

/**
 * @brief Force immediate health check update
 */
void health_check_trigger_update(void);

/**
 * @brief Get health status as string
 */
const char* health_status_to_string(health_status_t status);

/**
 * @brief Write health status to file (Prometheus format)
 * @param path File path
 * @return RESULT_OK on success
 */
result_t health_check_write_file(const char *path);

/**
 * @brief Get health as JSON string
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Number of bytes written, or -1 on error
 */
int health_check_to_json(char *buffer, size_t buffer_size);

/**
 * @brief Get health as Prometheus metrics format
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Number of bytes written, or -1 on error
 */
int health_check_to_prometheus(char *buffer, size_t buffer_size);

#endif /* HEALTH_CHECK_H */
