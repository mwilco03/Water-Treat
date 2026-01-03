/**
 * @file db_modules.c
 * @brief Module and sensor database operations
 */

#include "db_modules.h"
#include "utils/logger.h"

/* ============================================================================
 * SQL String Constants
 * ============================================================================
 * Centralized SQL fragments to reduce duplication and prevent typos.
 * Column order must be consistent with map_row_to_module().
 */
#define MODULE_COLUMNS \
    "id, slot, subslot, name, module_type, module_ident, submodule_ident, status"

#define MODULE_SELECT "SELECT " MODULE_COLUMNS " FROM modules"

/* ============================================================================
 * Row Mapping Helpers
 * ============================================================================
 * Extract common code for mapping SQLite rows to structs.
 */

/**
 * Map a single row from a modules query to db_module_t struct.
 * Column order must match MODULE_COLUMNS.
 */
static void map_row_to_module(sqlite3_stmt *stmt, db_module_t *module) {
    module->id = sqlite3_column_int(stmt, 0);
    module->slot = sqlite3_column_int(stmt, 1);
    module->subslot = sqlite3_column_int(stmt, 2);
    SAFE_STRNCPY(module->name, (const char*)sqlite3_column_text(stmt, 3), sizeof(module->name));
    SAFE_STRNCPY(module->module_type, (const char*)sqlite3_column_text(stmt, 4), sizeof(module->module_type));
    module->module_ident = sqlite3_column_int(stmt, 5);
    module->submodule_ident = sqlite3_column_int(stmt, 6);
    SAFE_STRNCPY(module->status, (const char*)sqlite3_column_text(stmt, 7), sizeof(module->status));
}

/* ============================================================================
 * Module CRUD Operations
 * ========================================================================== */

result_t db_module_create(database_t *db, db_module_t *module, int *module_id) {
    CHECK_NULL(db); CHECK_NULL(module); CHECK_NULL(module_id);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "INSERT INTO modules (slot, subslot, name, module_type, module_ident, submodule_ident, status) VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("Prepare failed: %s", sqlite3_errmsg(db->db));
        return RESULT_ERROR;
    }
    
    sqlite3_bind_int(stmt, 1, module->slot);
    sqlite3_bind_int(stmt, 2, module->subslot);
    sqlite3_bind_text(stmt, 3, module->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, module->module_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, module->module_ident);
    sqlite3_bind_int(stmt, 6, module->submodule_ident);
    sqlite3_bind_text(stmt, 7, module->status[0] ? module->status : STATUS_INACTIVE, -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        LOG_ERROR("Insert module failed: %s", sqlite3_errmsg(db->db));
        return RESULT_ERROR;
    }
    
    *module_id = (int)sqlite3_last_insert_rowid(db->db);
    module->id = *module_id;
    
    // Create sensor_status entry
    const char *status_sql = "INSERT INTO sensor_status (module_id, status) VALUES (?, 'unknown');";
    sqlite3_stmt *status_stmt;
    if (sqlite3_prepare_v2(db->db, status_sql, -1, &status_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(status_stmt, 1, *module_id);
        sqlite3_step(status_stmt);
        sqlite3_finalize(status_stmt);
    }
    
    LOG_INFO("Created module %d: %s (slot %d)", *module_id, module->name, module->slot);
    return RESULT_OK;
}

result_t db_module_update(database_t *db, db_module_t *module) {
    CHECK_NULL(db); CHECK_NULL(module);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "UPDATE modules SET slot=?, subslot=?, name=?, module_type=?, module_ident=?, submodule_ident=?, status=?, updated_at=datetime('now') WHERE id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    sqlite3_bind_int(stmt, 1, module->slot);
    sqlite3_bind_int(stmt, 2, module->subslot);
    sqlite3_bind_text(stmt, 3, module->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, module->module_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, module->module_ident);
    sqlite3_bind_int(stmt, 6, module->submodule_ident);
    sqlite3_bind_text(stmt, 7, module->status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 8, module->id);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE ? RESULT_OK : RESULT_ERROR;
}

result_t db_module_delete(database_t *db, int module_id) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "DELETE FROM modules WHERE id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, module_id);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE && sqlite3_changes(db->db) > 0) {
        LOG_INFO("Deleted module %d", module_id);
        return RESULT_OK;
    }
    return RESULT_NOT_FOUND;
}

result_t db_module_get(database_t *db, int module_id, db_module_t *module) {
    CHECK_NULL(db); CHECK_NULL(module);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    const char *sql = MODULE_SELECT " WHERE id=?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, module_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }

    map_row_to_module(stmt, module);
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_module_get_by_slot(database_t *db, int slot, db_module_t *module) {
    CHECK_NULL(db); CHECK_NULL(module);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    const char *sql = MODULE_SELECT " WHERE slot=?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, slot);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }

    map_row_to_module(stmt, module);
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_module_list(database_t *db, db_module_t **modules, int *count) {
    CHECK_NULL(db); CHECK_NULL(modules); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    *modules = NULL;
    *count = 0;

    /* Count first */
    const char *count_sql = "SELECT COUNT(*) FROM modules;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, count_sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;

    int total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (total == 0) return RESULT_OK;

    *modules = calloc(total, sizeof(db_module_t));
    if (!*modules) return RESULT_NO_MEMORY;

    const char *sql = MODULE_SELECT " ORDER BY slot;";
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(*modules);
        *modules = NULL;
        return RESULT_ERROR;
    }

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < total) {
        map_row_to_module(stmt, &(*modules)[idx]);
        idx++;
    }

    sqlite3_finalize(stmt);
    *count = idx;
    return RESULT_OK;
}

result_t db_module_count(database_t *db, int *count) {
    CHECK_NULL(db); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT COUNT(*) FROM modules;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    *count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) *count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

/* ============================================================================
 * Physical Sensor Operations
 * ========================================================================== */

result_t db_physical_sensor_create(database_t *db, db_physical_sensor_t *sensor) {
    CHECK_NULL(db); CHECK_NULL(sensor);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "INSERT INTO physical_sensors (module_id, sensor_type, hardware_type, interface, address, bus, channel, resolution, unit, min_value, max_value, poll_rate_ms, timeout_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    sqlite3_bind_int(stmt, 1, sensor->module_id);
    sqlite3_bind_text(stmt, 2, sensor->sensor_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, sensor->hardware_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, sensor->interface, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, sensor->address, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, sensor->bus);
    sqlite3_bind_int(stmt, 7, sensor->channel);
    sqlite3_bind_double(stmt, 8, sensor->resolution);
    sqlite3_bind_text(stmt, 9, sensor->unit, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 10, sensor->min_value);
    sqlite3_bind_double(stmt, 11, sensor->max_value);
    sqlite3_bind_int(stmt, 12, sensor->poll_rate_ms);
    sqlite3_bind_int(stmt, 13, sensor->timeout_ms);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE) {
        sensor->id = (int)sqlite3_last_insert_rowid(db->db);
        return RESULT_OK;
    }
    return RESULT_ERROR;
}

result_t db_physical_sensor_get(database_t *db, int module_id, db_physical_sensor_t *sensor) {
    CHECK_NULL(db); CHECK_NULL(sensor);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT id, module_id, sensor_type, hardware_type, interface, address, bus, channel, resolution, unit, min_value, max_value, poll_rate_ms, timeout_ms FROM physical_sensors WHERE module_id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, module_id);
    
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }
    
    sensor->id = sqlite3_column_int(stmt, 0);
    sensor->module_id = sqlite3_column_int(stmt, 1);
    SAFE_STRNCPY(sensor->sensor_type, (const char*)sqlite3_column_text(stmt, 2), sizeof(sensor->sensor_type));
    SAFE_STRNCPY(sensor->hardware_type, (const char*)sqlite3_column_text(stmt, 3), sizeof(sensor->hardware_type));
    SAFE_STRNCPY(sensor->interface, (const char*)sqlite3_column_text(stmt, 4), sizeof(sensor->interface));
    SAFE_STRNCPY(sensor->address, (const char*)sqlite3_column_text(stmt, 5), sizeof(sensor->address));
    sensor->bus = sqlite3_column_int(stmt, 6);
    sensor->channel = sqlite3_column_int(stmt, 7);
    sensor->resolution = sqlite3_column_double(stmt, 8);
    SAFE_STRNCPY(sensor->unit, (const char*)sqlite3_column_text(stmt, 9), sizeof(sensor->unit));
    sensor->min_value = sqlite3_column_double(stmt, 10);
    sensor->max_value = sqlite3_column_double(stmt, 11);
    sensor->poll_rate_ms = sqlite3_column_int(stmt, 12);
    sensor->timeout_ms = sqlite3_column_int(stmt, 13);
    
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_physical_sensor_update(database_t *db, db_physical_sensor_t *sensor) {
    CHECK_NULL(db); CHECK_NULL(sensor);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "UPDATE physical_sensors SET sensor_type=?, hardware_type=?, interface=?, address=?, bus=?, channel=?, resolution=?, unit=?, min_value=?, max_value=?, poll_rate_ms=?, timeout_ms=? WHERE module_id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    sqlite3_bind_text(stmt, 1, sensor->sensor_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, sensor->hardware_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, sensor->interface, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, sensor->address, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, sensor->bus);
    sqlite3_bind_int(stmt, 6, sensor->channel);
    sqlite3_bind_double(stmt, 7, sensor->resolution);
    sqlite3_bind_text(stmt, 8, sensor->unit, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 9, sensor->min_value);
    sqlite3_bind_double(stmt, 10, sensor->max_value);
    sqlite3_bind_int(stmt, 11, sensor->poll_rate_ms);
    sqlite3_bind_int(stmt, 12, sensor->timeout_ms);
    sqlite3_bind_int(stmt, 13, sensor->module_id);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? RESULT_OK : RESULT_ERROR;
}

/* ============================================================================
 * ADC Sensor Operations
 * ========================================================================== */

result_t db_adc_sensor_create(database_t *db, db_adc_sensor_t *sensor) {
    CHECK_NULL(db); CHECK_NULL(sensor);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "INSERT INTO adc_sensors (module_id, adc_type, interface, address, bus, channel, gain, reference_voltage, unit, raw_min, raw_max, eng_min, eng_max, poll_rate_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    sqlite3_bind_int(stmt, 1, sensor->module_id);
    sqlite3_bind_text(stmt, 2, sensor->adc_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, sensor->interface, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, sensor->address, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, sensor->bus);
    sqlite3_bind_int(stmt, 6, sensor->channel);
    sqlite3_bind_int(stmt, 7, sensor->gain);
    sqlite3_bind_double(stmt, 8, sensor->reference_voltage);
    sqlite3_bind_text(stmt, 9, sensor->unit, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 10, sensor->raw_min);
    sqlite3_bind_int(stmt, 11, sensor->raw_max);
    sqlite3_bind_double(stmt, 12, sensor->eng_min);
    sqlite3_bind_double(stmt, 13, sensor->eng_max);
    sqlite3_bind_int(stmt, 14, sensor->poll_rate_ms);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE) {
        sensor->id = (int)sqlite3_last_insert_rowid(db->db);
        return RESULT_OK;
    }
    return RESULT_ERROR;
}

result_t db_adc_sensor_get(database_t *db, int module_id, db_adc_sensor_t *sensor) {
    CHECK_NULL(db); CHECK_NULL(sensor);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT id, module_id, adc_type, interface, address, bus, channel, gain, reference_voltage, unit, raw_min, raw_max, eng_min, eng_max, poll_rate_ms FROM adc_sensors WHERE module_id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, module_id);
    
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }
    
    sensor->id = sqlite3_column_int(stmt, 0);
    sensor->module_id = sqlite3_column_int(stmt, 1);
    SAFE_STRNCPY(sensor->adc_type, (const char*)sqlite3_column_text(stmt, 2), sizeof(sensor->adc_type));
    SAFE_STRNCPY(sensor->interface, (const char*)sqlite3_column_text(stmt, 3), sizeof(sensor->interface));
    SAFE_STRNCPY(sensor->address, (const char*)sqlite3_column_text(stmt, 4), sizeof(sensor->address));
    sensor->bus = sqlite3_column_int(stmt, 5);
    sensor->channel = sqlite3_column_int(stmt, 6);
    sensor->gain = sqlite3_column_int(stmt, 7);
    sensor->reference_voltage = sqlite3_column_double(stmt, 8);
    SAFE_STRNCPY(sensor->unit, (const char*)sqlite3_column_text(stmt, 9), sizeof(sensor->unit));
    sensor->raw_min = sqlite3_column_int(stmt, 10);
    sensor->raw_max = sqlite3_column_int(stmt, 11);
    sensor->eng_min = sqlite3_column_double(stmt, 12);
    sensor->eng_max = sqlite3_column_double(stmt, 13);
    sensor->poll_rate_ms = sqlite3_column_int(stmt, 14);
    
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

/* ============================================================================
 * Web Poll Sensor Operations
 * ========================================================================== */

result_t db_web_poll_sensor_create(database_t *db, db_web_poll_sensor_t *sensor) {
    CHECK_NULL(db); CHECK_NULL(sensor);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "INSERT INTO web_poll_sensors (module_id, url, method, headers, json_path, poll_rate_ms, timeout_ms) VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    sqlite3_bind_int(stmt, 1, sensor->module_id);
    sqlite3_bind_text(stmt, 2, sensor->url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, sensor->method[0] ? sensor->method : "GET", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, sensor->headers, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, sensor->json_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, sensor->poll_rate_ms);
    sqlite3_bind_int(stmt, 7, sensor->timeout_ms);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE) {
        sensor->id = (int)sqlite3_last_insert_rowid(db->db);
        return RESULT_OK;
    }
    return RESULT_ERROR;
}

result_t db_web_poll_sensor_get(database_t *db, int module_id, db_web_poll_sensor_t *sensor) {
    CHECK_NULL(db); CHECK_NULL(sensor);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT id, module_id, url, method, headers, json_path, poll_rate_ms, timeout_ms FROM web_poll_sensors WHERE module_id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, module_id);
    
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }
    
    sensor->id = sqlite3_column_int(stmt, 0);
    sensor->module_id = sqlite3_column_int(stmt, 1);
    SAFE_STRNCPY(sensor->url, (const char*)sqlite3_column_text(stmt, 2), sizeof(sensor->url));
    SAFE_STRNCPY(sensor->method, (const char*)sqlite3_column_text(stmt, 3), sizeof(sensor->method));
    SAFE_STRNCPY(sensor->headers, (const char*)sqlite3_column_text(stmt, 4), sizeof(sensor->headers));
    SAFE_STRNCPY(sensor->json_path, (const char*)sqlite3_column_text(stmt, 5), sizeof(sensor->json_path));
    sensor->poll_rate_ms = sqlite3_column_int(stmt, 6);
    sensor->timeout_ms = sqlite3_column_int(stmt, 7);
    
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

/* ============================================================================
 * Calculated Sensor Operations
 * ========================================================================== */

result_t db_calculated_sensor_create(database_t *db, db_calculated_sensor_t *sensor) {
    CHECK_NULL(db); CHECK_NULL(sensor);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "INSERT INTO calculated_sensors (module_id, formula, input_sensors, unit, update_rate_ms) VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    sqlite3_bind_int(stmt, 1, sensor->module_id);
    sqlite3_bind_text(stmt, 2, sensor->formula, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, sensor->input_sensors, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, sensor->unit, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, sensor->update_rate_ms);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE) {
        sensor->id = (int)sqlite3_last_insert_rowid(db->db);
        return RESULT_OK;
    }
    return RESULT_ERROR;
}

result_t db_calculated_sensor_get(database_t *db, int module_id, db_calculated_sensor_t *sensor) {
    CHECK_NULL(db); CHECK_NULL(sensor);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT id, module_id, formula, input_sensors, unit, update_rate_ms FROM calculated_sensors WHERE module_id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, module_id);
    
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }
    
    sensor->id = sqlite3_column_int(stmt, 0);
    sensor->module_id = sqlite3_column_int(stmt, 1);
    SAFE_STRNCPY(sensor->formula, (const char*)sqlite3_column_text(stmt, 2), sizeof(sensor->formula));
    SAFE_STRNCPY(sensor->input_sensors, (const char*)sqlite3_column_text(stmt, 3), sizeof(sensor->input_sensors));
    SAFE_STRNCPY(sensor->unit, (const char*)sqlite3_column_text(stmt, 4), sizeof(sensor->unit));
    sensor->update_rate_ms = sqlite3_column_int(stmt, 5);
    
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

/* ============================================================================
 * Static Sensor Operations
 * ========================================================================== */

result_t db_static_sensor_create(database_t *db, db_static_sensor_t *sensor) {
    CHECK_NULL(db); CHECK_NULL(sensor);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "INSERT INTO static_sensors (module_id, value, unit, writable) VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    sqlite3_bind_int(stmt, 1, sensor->module_id);
    sqlite3_bind_double(stmt, 2, sensor->value);
    sqlite3_bind_text(stmt, 3, sensor->unit, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, sensor->writable ? 1 : 0);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE) {
        sensor->id = (int)sqlite3_last_insert_rowid(db->db);
        return RESULT_OK;
    }
    return RESULT_ERROR;
}

result_t db_static_sensor_get(database_t *db, int module_id, db_static_sensor_t *sensor) {
    CHECK_NULL(db); CHECK_NULL(sensor);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT id, module_id, value, unit, writable FROM static_sensors WHERE module_id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, module_id);
    
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }
    
    sensor->id = sqlite3_column_int(stmt, 0);
    sensor->module_id = sqlite3_column_int(stmt, 1);
    sensor->value = sqlite3_column_double(stmt, 2);
    SAFE_STRNCPY(sensor->unit, (const char*)sqlite3_column_text(stmt, 3), sizeof(sensor->unit));
    sensor->writable = sqlite3_column_int(stmt, 4) != 0;
    
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_static_sensor_set_value(database_t *db, int module_id, float value) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "UPDATE static_sensors SET value=? WHERE module_id=? AND writable=1;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_double(stmt, 1, value);
    sqlite3_bind_int(stmt, 2, module_id);
    
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db->db);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE && changes > 0) ? RESULT_OK : RESULT_ERROR;
}

/* ============================================================================
 * Sensor Status Operations
 * ========================================================================== */

result_t db_sensor_status_update(database_t *db, int module_id, float value, const char *status) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "INSERT OR REPLACE INTO sensor_status (module_id, value, status, last_update, consecutive_failures) VALUES (?, ?, ?, datetime('now'), CASE WHEN ? = 'ok' THEN 0 ELSE COALESCE((SELECT consecutive_failures FROM sensor_status WHERE module_id = ?) + 1, 1) END);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    sqlite3_bind_int(stmt, 1, module_id);
    sqlite3_bind_double(stmt, 2, value);
    sqlite3_bind_text(stmt, 3, status ? status : STATUS_OK, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, status ? status : STATUS_OK, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, module_id);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? RESULT_OK : RESULT_ERROR;
}

result_t db_sensor_status_get(database_t *db, int module_id, float *value, char *status, size_t status_size) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT value, status FROM sensor_status WHERE module_id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, module_id);
    
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }
    
    if (value) *value = sqlite3_column_double(stmt, 0);
    if (status) SAFE_STRNCPY(status, (const char*)sqlite3_column_text(stmt, 1), status_size);
    
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

/* ============================================================================
 * Data Logging Operations
 * ========================================================================== */

result_t db_sensor_log_insert(database_t *db, int module_id, float value, const char *status) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "INSERT INTO sensor_data_log (module_id, value, status) VALUES (?, ?, ?);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    sqlite3_bind_int(stmt, 1, module_id);
    sqlite3_bind_double(stmt, 2, value);
    sqlite3_bind_text(stmt, 3, status ? status : STATUS_OK, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? RESULT_OK : RESULT_ERROR;
}

result_t db_sensor_log_cleanup(database_t *db, int retention_days) {
    CHECK_NULL(db);
    if (!db->db || retention_days <= 0) return RESULT_INVALID_PARAM;
    
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM sensor_data_log WHERE timestamp < datetime('now', '-%d days');", retention_days);
    
    char *err = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &err);
    
    if (rc != SQLITE_OK) {
        LOG_ERROR("Log cleanup failed: %s", err);
        sqlite3_free(err);
        return RESULT_ERROR;
    }
    
    int deleted = sqlite3_changes(db->db);
    if (deleted > 0) LOG_INFO("Cleaned up %d old log entries", deleted);
    return RESULT_OK;
}
