#ifndef DB_MODULES_H
#define DB_MODULES_H

#include "common.h"
#include "database.h"

typedef struct {
    int id;
    int slot;
    int subslot;
    char name[MAX_NAME_LEN];
    char module_type[32];
    uint32_t module_ident;
    uint32_t submodule_ident;
    char status[16];
} db_module_t;

typedef struct {
    int id;
    int module_id;
    char sensor_type[32];
    char hardware_type[32];
    char interface[16];
    char address[32];
    int bus;
    int channel;
    float resolution;
    char unit[16];
    float min_value;
    float max_value;
    int poll_rate_ms;
    int timeout_ms;
} db_physical_sensor_t;

typedef struct {
    int id;
    int module_id;
    char adc_type[32];
    char interface[16];
    char address[32];
    int bus;
    int channel;
    int gain;
    float reference_voltage;
    char unit[16];
    int raw_min;
    int raw_max;
    float eng_min;
    float eng_max;
    int poll_rate_ms;
} db_adc_sensor_t;

typedef struct {
    int id;
    int module_id;
    char url[256];
    char method[16];
    char headers[256];
    char json_path[128];
    char auth_type[16];
    char auth_token[256];
    char unit[16];
    int poll_rate_ms;
    int timeout_ms;
} db_web_poll_sensor_t;

typedef struct {
    int id;
    int module_id;
    char formula[256];
    char input_sensors[256];
    char unit[16];
    int update_rate_ms;
} db_calculated_sensor_t;

typedef struct {
    int id;
    int module_id;
    float value;
    char unit[16];
    bool writable;
} db_static_sensor_t;

// Module CRUD operations
result_t db_module_create(database_t *db, db_module_t *module, int *module_id);
result_t db_module_update(database_t *db, db_module_t *module);
result_t db_module_delete(database_t *db, int module_id);
result_t db_module_get(database_t *db, int module_id, db_module_t *module);
result_t db_module_get_by_slot(database_t *db, int slot, db_module_t *module);
result_t db_module_list(database_t *db, db_module_t **modules, int *count);
result_t db_module_count(database_t *db, int *count);

// Physical sensor operations
result_t db_physical_sensor_create(database_t *db, db_physical_sensor_t *sensor);
result_t db_physical_sensor_get(database_t *db, int module_id, db_physical_sensor_t *sensor);
result_t db_physical_sensor_update(database_t *db, db_physical_sensor_t *sensor);

// ADC sensor operations
result_t db_adc_sensor_create(database_t *db, db_adc_sensor_t *sensor);
result_t db_adc_sensor_get(database_t *db, int module_id, db_adc_sensor_t *sensor);

// Web poll sensor operations
result_t db_web_poll_sensor_create(database_t *db, db_web_poll_sensor_t *sensor);
result_t db_web_poll_sensor_get(database_t *db, int module_id, db_web_poll_sensor_t *sensor);

// Calculated sensor operations
result_t db_calculated_sensor_create(database_t *db, db_calculated_sensor_t *sensor);
result_t db_calculated_sensor_get(database_t *db, int module_id, db_calculated_sensor_t *sensor);

// Static sensor operations
result_t db_static_sensor_create(database_t *db, db_static_sensor_t *sensor);
result_t db_static_sensor_get(database_t *db, int module_id, db_static_sensor_t *sensor);

// Sensor status operations
result_t db_sensor_status_update(database_t *db, int module_id, float value, const char *status);
result_t db_sensor_status_get(database_t *db, int module_id, float *value, char *status, size_t status_size);

/**
 * Module with status - combined struct for efficient JOIN query
 * Eliminates N+1 query problem when listing modules with their status
 */
typedef struct {
    db_module_t module;
    float value;
    char sensor_status[16];
} db_module_with_status_t;

/**
 * List all modules with their sensor status in a single query (uses SQL JOIN)
 * More efficient than calling db_module_list() + db_sensor_status_get() per module
 */
result_t db_module_list_with_status(database_t *db, db_module_with_status_t **modules, int *count);

// Sensor log operations
result_t db_sensor_log_insert(database_t *db, int module_id, float value, const char *status);
result_t db_sensor_log_cleanup(database_t *db, int retention_days);

/**
 * Batch insert sensor log entries within a single transaction.
 * Much faster than individual inserts (10-100x for large batches).
 * @param db Database handle
 * @param module_ids Array of module IDs
 * @param values Array of sensor values
 * @param statuses Array of status strings (can be NULL for all "ok")
 * @param count Number of entries to insert
 * @return RESULT_OK on success
 */
result_t db_sensor_log_insert_batch(database_t *db, const int *module_ids,
                                     const float *values, const char **statuses, int count);

// Utility
void db_module_free_list(db_module_t *modules);

#endif
