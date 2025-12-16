#ifndef DATA_LOGGER_H
#define DATA_LOGGER_H

#include "common.h"
#include "db/database.h"

typedef struct {
    bool enabled;
    bool local_enabled;
    bool remote_enabled;
    int interval_seconds;
    int retention_days;
    char device_name[MAX_NAME_LEN];
    char remote_url[MAX_PATH_LEN];
    char api_key[MAX_CONFIG_VALUE_LEN];

    // Store & Forward Configuration
    bool queue_when_offline;        // Queue entries when remote unavailable (default: true)
    bool flush_on_reconnect;        // Flush queued entries when remote becomes available (default: true)
    int max_queue_age_seconds;      // Drop entries older than this (0=never, default: 3600)
} data_logger_config_t;

typedef struct {
    uint64_t total_logged;
    uint64_t total_remote_sent;
    uint64_t total_remote_failed;
    int queue_count;
    int queue_capacity;
    bool remote_available;
    int remote_failures;
} data_logger_stats_t;

result_t data_logger_init(database_t *db, const data_logger_config_t *config);
result_t data_logger_start(void);
result_t data_logger_stop(void);
void data_logger_shutdown(void);

result_t data_logger_log(int module_id, float value, const char *status);
result_t data_logger_log_batch(int *module_ids, float *values, const char **statuses, int count);
result_t data_logger_flush(void);
result_t data_logger_cleanup(int retention_days);

result_t data_logger_get_stats(data_logger_stats_t *stats);
result_t data_logger_set_remote(const char *url, const char *api_key);
result_t data_logger_enable(bool enabled);
result_t data_logger_set_interval(int seconds);
bool data_logger_is_running(void);

// Store & Forward Control
result_t data_logger_set_queue_mode(bool queue_when_offline, bool flush_on_reconnect);
result_t data_logger_force_flush(void);              // Force immediate flush attempt to remote
result_t data_logger_notify_connection(bool connected);  // Called when network state changes

#endif
