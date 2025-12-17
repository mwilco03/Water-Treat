#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

#define MAX_CONFIG_ENTRIES 256

typedef struct { char section[MAX_NAME_LEN]; char key[MAX_NAME_LEN]; char value[MAX_CONFIG_VALUE_LEN]; } config_entry_t;
typedef struct { config_entry_t entries[MAX_CONFIG_ENTRIES]; int entry_count; char config_path[MAX_PATH_LEN]; bool modified; } config_manager_t;

typedef struct { char device_name[MAX_NAME_LEN]; char log_level[16]; char log_file[MAX_PATH_LEN]; bool daemon_mode; } system_config_t;
typedef struct { char interface[32]; char ip_address[16]; char netmask[16]; char gateway[16]; bool dhcp_enabled; } network_config_t;
typedef struct { char station_name[MAX_NAME_LEN]; uint16_t vendor_id; uint16_t device_id; char product_name[64]; uint32_t min_device_interval; bool enabled; } profinet_config_t;
typedef struct { char path[MAX_PATH_LEN]; bool create_if_missing; int busy_timeout_ms; } database_config_t;
typedef struct { bool enabled; int interval_seconds; int retention_days; int destination; char remote_url[MAX_PATH_LEN]; bool remote_enabled; } logging_config_t;
typedef struct { bool enabled; bool http_enabled; uint16_t http_port; char file_path[MAX_PATH_LEN]; int update_interval_seconds; } health_config_t;
typedef struct { system_config_t system; network_config_t network; profinet_config_t profinet; database_config_t database; logging_config_t logging; health_config_t health; } app_config_t;

result_t config_manager_init(config_manager_t *mgr);
void config_manager_destroy(config_manager_t *mgr);
result_t config_load_file(config_manager_t *mgr, const char *path);
result_t config_save_file(config_manager_t *mgr, const char *path);
result_t config_get_string(config_manager_t *mgr, const char *section, const char *key, char *value, size_t size);
result_t config_set_string(config_manager_t *mgr, const char *section, const char *key, const char *value);
result_t config_get_int(config_manager_t *mgr, const char *section, const char *key, int *value);
result_t config_get_bool(config_manager_t *mgr, const char *section, const char *key, bool *value);
result_t config_load_app_config(config_manager_t *mgr, app_config_t *config);
void config_get_defaults(app_config_t *config);

#endif
