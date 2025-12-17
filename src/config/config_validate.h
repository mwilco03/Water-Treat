/**
 * @file config_validate.h
 * @brief Configuration validation and remote loading
 */

#ifndef CONFIG_VALIDATE_H
#define CONFIG_VALIDATE_H

#include "config.h"

/* Validation result flags */
typedef enum {
    CONFIG_VALID = 0,
    CONFIG_WARN_DEFAULT_STATION_NAME = (1 << 0),
    CONFIG_WARN_DEFAULT_DEVICE_NAME  = (1 << 1),
    CONFIG_WARN_PROFINET_DISABLED    = (1 << 2),
    CONFIG_WARN_NO_SENSORS           = (1 << 3),
    CONFIG_WARN_HEALTH_DISABLED      = (1 << 4),
    CONFIG_ERROR_INVALID_PORT        = (1 << 8),
    CONFIG_ERROR_INVALID_INTERVAL    = (1 << 9),
    CONFIG_ERROR_MISSING_INTERFACE   = (1 << 10),
    CONFIG_ERROR_INVALID_DB_PATH     = (1 << 11),
} config_validation_flags_t;

/* Validation result structure */
typedef struct {
    uint32_t flags;
    int warning_count;
    int error_count;
    char messages[10][128];
    int message_count;
} config_validation_result_t;

/**
 * @brief Validate configuration and return warnings/errors
 * @param config Configuration to validate
 * @param result Output validation result
 * @return RESULT_OK if no errors (warnings allowed)
 */
result_t config_validate(const app_config_t *config, config_validation_result_t *result);

/**
 * @brief Log all validation warnings and errors
 * @param result Validation result to log
 */
void config_validation_log(const config_validation_result_t *result);

/**
 * @brief Check if this appears to be first run (unconfigured)
 * @param config Configuration to check
 * @return true if configuration appears to be defaults
 */
bool config_is_first_run(const app_config_t *config);

/**
 * @brief Load configuration from HTTP URL
 * @param url Remote URL to fetch config from
 * @param mgr Config manager to populate
 * @return RESULT_OK on success, RESULT_ERROR on failure
 *
 * Fetches INI-format config file from URL and parses it.
 * Falls back to local config if remote unavailable.
 */
result_t config_load_from_url(const char *url, config_manager_t *mgr);

/**
 * @brief Bootstrap configuration with optional remote URL
 * @param mgr Config manager
 * @param config App config to populate
 * @param bootstrap_url Optional URL to fetch config from (NULL for local only)
 * @param local_path Local config file path
 * @return RESULT_OK on success
 *
 * Order of precedence:
 * 1. Remote URL (if provided and reachable)
 * 2. Local config file
 * 3. Built-in defaults
 */
result_t config_bootstrap(config_manager_t *mgr, app_config_t *config,
                          const char *bootstrap_url, const char *local_path);

#endif /* CONFIG_VALIDATE_H */
