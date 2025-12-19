/**
 * @file db_actuators.c
 * @brief Actuator database operations for PROFINET output modules
 */

#include "db_actuators.h"
#include "utils/logger.h"

const char* actuator_type_to_string(actuator_type_t type) {
    switch (type) {
        case ACTUATOR_TYPE_RELAY: return "relay";
        case ACTUATOR_TYPE_PWM: return "pwm";
        case ACTUATOR_TYPE_LATCHING: return "latching";
        case ACTUATOR_TYPE_MOMENTARY: return "momentary";
        default: return "unknown";
    }
}

const char* safe_state_to_string(safe_state_t state) {
    switch (state) {
        case SAFE_STATE_OFF: return "off";
        case SAFE_STATE_ON: return "on";
        case SAFE_STATE_HOLD: return "hold";
        default: return "unknown";
    }
}

static actuator_type_t string_to_actuator_type(const char *str) {
    if (!str) return ACTUATOR_TYPE_RELAY;
    if (strcmp(str, "pwm") == 0) return ACTUATOR_TYPE_PWM;
    if (strcmp(str, "latching") == 0) return ACTUATOR_TYPE_LATCHING;
    if (strcmp(str, "momentary") == 0) return ACTUATOR_TYPE_MOMENTARY;
    return ACTUATOR_TYPE_RELAY;
}

static safe_state_t string_to_safe_state(const char *str) {
    if (!str) return SAFE_STATE_HOLD;
    if (strcmp(str, "off") == 0) return SAFE_STATE_OFF;
    if (strcmp(str, "on") == 0) return SAFE_STATE_ON;
    return SAFE_STATE_HOLD;
}

/* ============================================================================
 * Actuator CRUD Operations
 * ========================================================================== */

result_t db_actuator_create(database_t *db, db_actuator_t *actuator, int *actuator_id) {
    CHECK_NULL(db); CHECK_NULL(actuator); CHECK_NULL(actuator_id);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    const char *sql = "INSERT INTO actuators (slot, subslot, name, type, gpio_pin, gpio_chip, "
                      "active_low, safe_state, min_on_time_ms, max_on_time_ms, pwm_frequency_hz, "
                      "status, enabled) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("Prepare failed: %s", sqlite3_errmsg(db->db));
        return RESULT_ERROR;
    }

    sqlite3_bind_int(stmt, 1, actuator->slot);
    sqlite3_bind_int(stmt, 2, actuator->subslot);
    sqlite3_bind_text(stmt, 3, actuator->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, actuator_type_to_string(actuator->type), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, actuator->gpio_pin);
    sqlite3_bind_text(stmt, 6, actuator->gpio_chip, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, actuator->active_low ? 1 : 0);
    sqlite3_bind_text(stmt, 8, safe_state_to_string(actuator->safe_state), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, actuator->min_on_time_ms);
    sqlite3_bind_int(stmt, 10, actuator->max_on_time_ms);
    sqlite3_bind_int(stmt, 11, actuator->pwm_frequency_hz);
    sqlite3_bind_text(stmt, 12, actuator->status[0] ? actuator->status : "inactive", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 13, actuator->enabled ? 1 : 0);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("Insert actuator failed: %s", sqlite3_errmsg(db->db));
        return RESULT_ERROR;
    }

    *actuator_id = (int)sqlite3_last_insert_rowid(db->db);
    actuator->id = *actuator_id;

    // Create actuator_state entry
    const char *state_sql = "INSERT INTO actuator_state (actuator_id, state, pwm_duty) VALUES (?, 0, 0);";
    sqlite3_stmt *state_stmt;
    if (sqlite3_prepare_v2(db->db, state_sql, -1, &state_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(state_stmt, 1, *actuator_id);
        sqlite3_step(state_stmt);
        sqlite3_finalize(state_stmt);
    }

    LOG_INFO("Created actuator %d: %s (slot %d, gpio %d)", *actuator_id, actuator->name,
             actuator->slot, actuator->gpio_pin);
    return RESULT_OK;
}

result_t db_actuator_update(database_t *db, db_actuator_t *actuator) {
    CHECK_NULL(db); CHECK_NULL(actuator);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    const char *sql = "UPDATE actuators SET slot=?, subslot=?, name=?, type=?, gpio_pin=?, "
                      "gpio_chip=?, active_low=?, safe_state=?, min_on_time_ms=?, max_on_time_ms=?, "
                      "pwm_frequency_hz=?, status=?, enabled=?, updated_at=datetime('now') WHERE id=?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;

    sqlite3_bind_int(stmt, 1, actuator->slot);
    sqlite3_bind_int(stmt, 2, actuator->subslot);
    sqlite3_bind_text(stmt, 3, actuator->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, actuator_type_to_string(actuator->type), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, actuator->gpio_pin);
    sqlite3_bind_text(stmt, 6, actuator->gpio_chip, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, actuator->active_low ? 1 : 0);
    sqlite3_bind_text(stmt, 8, safe_state_to_string(actuator->safe_state), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, actuator->min_on_time_ms);
    sqlite3_bind_int(stmt, 10, actuator->max_on_time_ms);
    sqlite3_bind_int(stmt, 11, actuator->pwm_frequency_hz);
    sqlite3_bind_text(stmt, 12, actuator->status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 13, actuator->enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 14, actuator->id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE ? RESULT_OK : RESULT_ERROR;
}

result_t db_actuator_delete(database_t *db, int actuator_id) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    const char *sql = "DELETE FROM actuators WHERE id=?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, actuator_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE && sqlite3_changes(db->db) > 0) {
        LOG_INFO("Deleted actuator %d", actuator_id);
        return RESULT_OK;
    }
    return RESULT_NOT_FOUND;
}

result_t db_actuator_get(database_t *db, int actuator_id, db_actuator_t *actuator) {
    CHECK_NULL(db); CHECK_NULL(actuator);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    const char *sql = "SELECT id, slot, subslot, name, type, gpio_pin, gpio_chip, active_low, "
                      "safe_state, min_on_time_ms, max_on_time_ms, pwm_frequency_hz, status, enabled "
                      "FROM actuators WHERE id=?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, actuator_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }

    actuator->id = sqlite3_column_int(stmt, 0);
    actuator->slot = sqlite3_column_int(stmt, 1);
    actuator->subslot = sqlite3_column_int(stmt, 2);
    SAFE_STRNCPY(actuator->name, (const char*)sqlite3_column_text(stmt, 3), sizeof(actuator->name));
    actuator->type = string_to_actuator_type((const char*)sqlite3_column_text(stmt, 4));
    actuator->gpio_pin = sqlite3_column_int(stmt, 5);
    SAFE_STRNCPY(actuator->gpio_chip, (const char*)sqlite3_column_text(stmt, 6), sizeof(actuator->gpio_chip));
    actuator->active_low = sqlite3_column_int(stmt, 7) != 0;
    actuator->safe_state = string_to_safe_state((const char*)sqlite3_column_text(stmt, 8));
    actuator->min_on_time_ms = sqlite3_column_int(stmt, 9);
    actuator->max_on_time_ms = sqlite3_column_int(stmt, 10);
    actuator->pwm_frequency_hz = sqlite3_column_int(stmt, 11);
    SAFE_STRNCPY(actuator->status, (const char*)sqlite3_column_text(stmt, 12), sizeof(actuator->status));
    actuator->enabled = sqlite3_column_int(stmt, 13) != 0;

    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_actuator_get_by_slot(database_t *db, int slot, db_actuator_t *actuator) {
    CHECK_NULL(db); CHECK_NULL(actuator);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    const char *sql = "SELECT id, slot, subslot, name, type, gpio_pin, gpio_chip, active_low, "
                      "safe_state, min_on_time_ms, max_on_time_ms, pwm_frequency_hz, status, enabled "
                      "FROM actuators WHERE slot=?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, slot);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }

    actuator->id = sqlite3_column_int(stmt, 0);
    actuator->slot = sqlite3_column_int(stmt, 1);
    actuator->subslot = sqlite3_column_int(stmt, 2);
    SAFE_STRNCPY(actuator->name, (const char*)sqlite3_column_text(stmt, 3), sizeof(actuator->name));
    actuator->type = string_to_actuator_type((const char*)sqlite3_column_text(stmt, 4));
    actuator->gpio_pin = sqlite3_column_int(stmt, 5);
    SAFE_STRNCPY(actuator->gpio_chip, (const char*)sqlite3_column_text(stmt, 6), sizeof(actuator->gpio_chip));
    actuator->active_low = sqlite3_column_int(stmt, 7) != 0;
    actuator->safe_state = string_to_safe_state((const char*)sqlite3_column_text(stmt, 8));
    actuator->min_on_time_ms = sqlite3_column_int(stmt, 9);
    actuator->max_on_time_ms = sqlite3_column_int(stmt, 10);
    actuator->pwm_frequency_hz = sqlite3_column_int(stmt, 11);
    SAFE_STRNCPY(actuator->status, (const char*)sqlite3_column_text(stmt, 12), sizeof(actuator->status));
    actuator->enabled = sqlite3_column_int(stmt, 13) != 0;

    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_actuator_list(database_t *db, db_actuator_t **actuators, int *count) {
    CHECK_NULL(db); CHECK_NULL(actuators); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    *actuators = NULL;
    *count = 0;

    // Count first
    const char *count_sql = "SELECT COUNT(*) FROM actuators;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, count_sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;

    int total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (total == 0) return RESULT_OK;

    *actuators = calloc(total, sizeof(db_actuator_t));
    if (!*actuators) return RESULT_NO_MEMORY;

    const char *sql = "SELECT id, slot, subslot, name, type, gpio_pin, gpio_chip, active_low, "
                      "safe_state, min_on_time_ms, max_on_time_ms, pwm_frequency_hz, status, enabled "
                      "FROM actuators ORDER BY slot;";
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(*actuators);
        *actuators = NULL;
        return RESULT_ERROR;
    }

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < total) {
        (*actuators)[idx].id = sqlite3_column_int(stmt, 0);
        (*actuators)[idx].slot = sqlite3_column_int(stmt, 1);
        (*actuators)[idx].subslot = sqlite3_column_int(stmt, 2);
        SAFE_STRNCPY((*actuators)[idx].name, (const char*)sqlite3_column_text(stmt, 3), sizeof((*actuators)[idx].name));
        (*actuators)[idx].type = string_to_actuator_type((const char*)sqlite3_column_text(stmt, 4));
        (*actuators)[idx].gpio_pin = sqlite3_column_int(stmt, 5);
        SAFE_STRNCPY((*actuators)[idx].gpio_chip, (const char*)sqlite3_column_text(stmt, 6), sizeof((*actuators)[idx].gpio_chip));
        (*actuators)[idx].active_low = sqlite3_column_int(stmt, 7) != 0;
        (*actuators)[idx].safe_state = string_to_safe_state((const char*)sqlite3_column_text(stmt, 8));
        (*actuators)[idx].min_on_time_ms = sqlite3_column_int(stmt, 9);
        (*actuators)[idx].max_on_time_ms = sqlite3_column_int(stmt, 10);
        (*actuators)[idx].pwm_frequency_hz = sqlite3_column_int(stmt, 11);
        SAFE_STRNCPY((*actuators)[idx].status, (const char*)sqlite3_column_text(stmt, 12), sizeof((*actuators)[idx].status));
        (*actuators)[idx].enabled = sqlite3_column_int(stmt, 13) != 0;
        idx++;
    }

    sqlite3_finalize(stmt);
    *count = idx;
    return RESULT_OK;
}

result_t db_actuator_count(database_t *db, int *count) {
    CHECK_NULL(db); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    const char *sql = "SELECT COUNT(*) FROM actuators;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;

    *count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

/* ============================================================================
 * Actuator State Operations
 * ========================================================================== */

result_t db_actuator_state_update(database_t *db, int actuator_id, bool state, int pwm_duty) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    const char *sql = "UPDATE actuator_state SET state=?, pwm_duty=?, last_state_change=datetime('now') "
                      "WHERE actuator_id=?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;

    sqlite3_bind_int(stmt, 1, state ? 1 : 0);
    sqlite3_bind_int(stmt, 2, pwm_duty);
    sqlite3_bind_int(stmt, 3, actuator_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE ? RESULT_OK : RESULT_ERROR;
}

result_t db_actuator_state_get(database_t *db, int actuator_id, db_actuator_state_t *state) {
    CHECK_NULL(db); CHECK_NULL(state);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    const char *sql = "SELECT actuator_id, state, pwm_duty, total_on_time_ms, cycle_count "
                      "FROM actuator_state WHERE actuator_id=?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, actuator_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }

    state->actuator_id = sqlite3_column_int(stmt, 0);
    state->state = sqlite3_column_int(stmt, 1) != 0;
    state->pwm_duty = sqlite3_column_int(stmt, 2);
    state->total_on_time_ms = sqlite3_column_int64(stmt, 3);
    state->cycle_count = sqlite3_column_int(stmt, 4);

    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_actuator_state_increment_cycle(database_t *db, int actuator_id) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    const char *sql = "UPDATE actuator_state SET cycle_count = cycle_count + 1 WHERE actuator_id=?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, actuator_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE ? RESULT_OK : RESULT_ERROR;
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

void db_actuator_free_list(db_actuator_t *actuators) {
    if (actuators) {
        free(actuators);
    }
}

/* ============================================================================
 * GPIO Pin Conflict Detection
 * ========================================================================== */

result_t db_actuator_gpio_conflict_check(database_t *db, int gpio_pin,
                                         const char *gpio_chip,
                                         int exclude_actuator_id,
                                         gpio_conflict_t *conflict) {
    CHECK_NULL(db); CHECK_NULL(conflict);
    if (!db->db) return RESULT_NOT_INITIALIZED;

    memset(conflict, 0, sizeof(*conflict));
    conflict->has_conflict = false;

    const char *chip = gpio_chip && gpio_chip[0] ? gpio_chip : "gpiochip0";
    char gpio_str[16];
    snprintf(gpio_str, sizeof(gpio_str), "%d", gpio_pin);

    /* Query for any actuator using this GPIO pin on the same chip */
    const char *sql = "SELECT id, name FROM actuators "
                      "WHERE gpio_pin = ? AND gpio_chip = ? AND id != ?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("Prepare failed: %s", sqlite3_errmsg(db->db));
        return RESULT_ERROR;
    }

    sqlite3_bind_int(stmt, 1, gpio_pin);
    sqlite3_bind_text(stmt, 2, chip, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, exclude_actuator_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        /* Found a conflict with actuator */
        conflict->has_conflict = true;
        conflict->conflict_type = 0;  /* actuator */
        conflict->conflicting_actuator_id = sqlite3_column_int(stmt, 0);
        const char *name = (const char*)sqlite3_column_text(stmt, 1);
        if (name) {
            SAFE_STRNCPY(conflict->conflicting_name, name, sizeof(conflict->conflicting_name));
        }
        LOG_DEBUG("GPIO conflict: pin %d on %s already used by actuator %d (%s)",
                  gpio_pin, chip, conflict->conflicting_actuator_id, conflict->conflicting_name);
        sqlite3_finalize(stmt);
        return RESULT_OK;
    }
    sqlite3_finalize(stmt);

    /* Also check sensors that use GPIO (DHT22, float switches, etc.)
     * Sensors store GPIO pin in 'address' field when interface is GPIO-based */
    const char *sensor_sql =
        "SELECT m.name FROM physical_sensors ps "
        "JOIN modules m ON ps.module_id = m.id "
        "WHERE ps.address = ? AND ps.sensor_type IN ('DHT22', 'DHT11', 'FLOAT_SWITCH', 'GPIO');";

    if (sqlite3_prepare_v2(db->db, sensor_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, gpio_str, -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            /* Found a conflict with sensor */
            conflict->has_conflict = true;
            conflict->conflict_type = 1;  /* sensor */
            conflict->conflicting_actuator_id = -1;  /* Indicate sensor conflict */
            const char *name = (const char*)sqlite3_column_text(stmt, 0);
            if (name) {
                SAFE_STRNCPY(conflict->conflicting_name, name, sizeof(conflict->conflicting_name));
            }
            LOG_DEBUG("GPIO conflict: pin %d already used by sensor %s",
                      gpio_pin, conflict->conflicting_name);
        }
        sqlite3_finalize(stmt);
    }

    return RESULT_OK;
}
