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

#endif
