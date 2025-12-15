/**
 * @file db_alarms.c
 * @brief Alarm rules and history database operations
 */

#include "db_alarms.h"
#include "utils/logger.h"

/* ============================================================================
 * Alarm Rule Operations
 * ========================================================================== */

result_t db_alarm_rule_create(database_t *db, db_alarm_rule_t *rule, int *rule_id) {
    CHECK_NULL(db); CHECK_NULL(rule); CHECK_NULL(rule_id);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "INSERT INTO alarm_rules (module_id, name, condition, threshold_high, threshold_low, severity, enabled, auto_clear, hysteresis_percent) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("Prepare failed: %s", sqlite3_errmsg(db->db));
        return RESULT_ERROR;
    }
    
    sqlite3_bind_int(stmt, 1, rule->module_id);
    sqlite3_bind_text(stmt, 2, rule->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, (int)rule->condition);
    sqlite3_bind_double(stmt, 4, rule->threshold_high);
    sqlite3_bind_double(stmt, 5, rule->threshold_low);
    sqlite3_bind_int(stmt, 6, (int)rule->severity);
    sqlite3_bind_int(stmt, 7, rule->enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 8, rule->auto_clear ? 1 : 0);
    sqlite3_bind_int(stmt, 9, rule->hysteresis_percent);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        LOG_ERROR("Insert alarm rule failed: %s", sqlite3_errmsg(db->db));
        return RESULT_ERROR;
    }
    
    *rule_id = (int)sqlite3_last_insert_rowid(db->db);
    rule->id = *rule_id;
    
    LOG_INFO("Created alarm rule %d: %s for module %d", *rule_id, rule->name, rule->module_id);
    return RESULT_OK;
}

result_t db_alarm_rule_update(database_t *db, db_alarm_rule_t *rule) {
    CHECK_NULL(db); CHECK_NULL(rule);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "UPDATE alarm_rules SET module_id=?, name=?, condition=?, threshold_high=?, threshold_low=?, severity=?, enabled=?, auto_clear=?, hysteresis_percent=? WHERE id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    sqlite3_bind_int(stmt, 1, rule->module_id);
    sqlite3_bind_text(stmt, 2, rule->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, (int)rule->condition);
    sqlite3_bind_double(stmt, 4, rule->threshold_high);
    sqlite3_bind_double(stmt, 5, rule->threshold_low);
    sqlite3_bind_int(stmt, 6, (int)rule->severity);
    sqlite3_bind_int(stmt, 7, rule->enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 8, rule->auto_clear ? 1 : 0);
    sqlite3_bind_int(stmt, 9, rule->hysteresis_percent);
    sqlite3_bind_int(stmt, 10, rule->id);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE ? RESULT_OK : RESULT_ERROR;
}

result_t db_alarm_rule_delete(database_t *db, int rule_id) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "DELETE FROM alarm_rules WHERE id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, rule_id);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE && sqlite3_changes(db->db) > 0) {
        LOG_INFO("Deleted alarm rule %d", rule_id);
        return RESULT_OK;
    }
    return RESULT_NOT_FOUND;
}

result_t db_alarm_rule_get(database_t *db, int rule_id, db_alarm_rule_t *rule) {
    CHECK_NULL(db); CHECK_NULL(rule);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT id, module_id, name, condition, threshold_high, threshold_low, severity, enabled, auto_clear, hysteresis_percent FROM alarm_rules WHERE id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, rule_id);
    
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }
    
    rule->id = sqlite3_column_int(stmt, 0);
    rule->module_id = sqlite3_column_int(stmt, 1);
    SAFE_STRNCPY(rule->name, (const char*)sqlite3_column_text(stmt, 2), sizeof(rule->name));
    rule->condition = (alarm_condition_t)sqlite3_column_int(stmt, 3);
    rule->threshold_high = sqlite3_column_double(stmt, 4);
    rule->threshold_low = sqlite3_column_double(stmt, 5);
    rule->severity = (alarm_severity_t)sqlite3_column_int(stmt, 6);
    rule->enabled = sqlite3_column_int(stmt, 7) != 0;
    rule->auto_clear = sqlite3_column_int(stmt, 8) != 0;
    rule->hysteresis_percent = sqlite3_column_int(stmt, 9);
    
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_alarm_rule_list(database_t *db, db_alarm_rule_t **rules, int *count) {
    CHECK_NULL(db); CHECK_NULL(rules); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    *rules = NULL;
    *count = 0;
    
    const char *count_sql = "SELECT COUNT(*) FROM alarm_rules;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, count_sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    int total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    
    if (total == 0) return RESULT_OK;
    
    *rules = calloc(total, sizeof(db_alarm_rule_t));
    if (!*rules) return RESULT_NO_MEMORY;
    
    const char *sql = "SELECT id, module_id, name, condition, threshold_high, threshold_low, severity, enabled, auto_clear, hysteresis_percent FROM alarm_rules ORDER BY module_id, id;";
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(*rules);
        *rules = NULL;
        return RESULT_ERROR;
    }
    
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < total) {
        (*rules)[idx].id = sqlite3_column_int(stmt, 0);
        (*rules)[idx].module_id = sqlite3_column_int(stmt, 1);
        SAFE_STRNCPY((*rules)[idx].name, (const char*)sqlite3_column_text(stmt, 2), sizeof((*rules)[idx].name));
        (*rules)[idx].condition = (alarm_condition_t)sqlite3_column_int(stmt, 3);
        (*rules)[idx].threshold_high = sqlite3_column_double(stmt, 4);
        (*rules)[idx].threshold_low = sqlite3_column_double(stmt, 5);
        (*rules)[idx].severity = (alarm_severity_t)sqlite3_column_int(stmt, 6);
        (*rules)[idx].enabled = sqlite3_column_int(stmt, 7) != 0;
        (*rules)[idx].auto_clear = sqlite3_column_int(stmt, 8) != 0;
        (*rules)[idx].hysteresis_percent = sqlite3_column_int(stmt, 9);
        idx++;
    }
    
    sqlite3_finalize(stmt);
    *count = idx;
    return RESULT_OK;
}

result_t db_alarm_rule_list_by_module(database_t *db, int module_id, db_alarm_rule_t **rules, int *count) {
    CHECK_NULL(db); CHECK_NULL(rules); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    *rules = NULL;
    *count = 0;
    
    const char *sql = "SELECT id, module_id, name, condition, threshold_high, threshold_low, severity, enabled, auto_clear, hysteresis_percent FROM alarm_rules WHERE module_id=? ORDER BY id;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, module_id);
    
    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) row_count++;
    sqlite3_reset(stmt);
    
    if (row_count == 0) {
        sqlite3_finalize(stmt);
        return RESULT_OK;
    }
    
    *rules = calloc(row_count, sizeof(db_alarm_rule_t));
    if (!*rules) {
        sqlite3_finalize(stmt);
        return RESULT_NO_MEMORY;
    }
    
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < row_count) {
        (*rules)[idx].id = sqlite3_column_int(stmt, 0);
        (*rules)[idx].module_id = sqlite3_column_int(stmt, 1);
        SAFE_STRNCPY((*rules)[idx].name, (const char*)sqlite3_column_text(stmt, 2), sizeof((*rules)[idx].name));
        (*rules)[idx].condition = (alarm_condition_t)sqlite3_column_int(stmt, 3);
        (*rules)[idx].threshold_high = sqlite3_column_double(stmt, 4);
        (*rules)[idx].threshold_low = sqlite3_column_double(stmt, 5);
        (*rules)[idx].severity = (alarm_severity_t)sqlite3_column_int(stmt, 6);
        (*rules)[idx].enabled = sqlite3_column_int(stmt, 7) != 0;
        (*rules)[idx].auto_clear = sqlite3_column_int(stmt, 8) != 0;
        (*rules)[idx].hysteresis_percent = sqlite3_column_int(stmt, 9);
        idx++;
    }
    
    sqlite3_finalize(stmt);
    *count = idx;
    return RESULT_OK;
}

result_t db_alarm_rule_set_enabled(database_t *db, int rule_id, bool enabled) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "UPDATE alarm_rules SET enabled=? WHERE id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 2, rule_id);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE ? RESULT_OK : RESULT_ERROR;
}

/* ============================================================================
 * Alarm History Operations
 * ========================================================================== */

result_t db_alarm_raise(database_t *db, db_alarm_history_t *alarm, int *alarm_id) {
    CHECK_NULL(db); CHECK_NULL(alarm); CHECK_NULL(alarm_id);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "INSERT INTO alarm_history (rule_id, module_id, severity, state, message, trigger_value) VALUES (?, ?, ?, 'active', ?, ?);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    sqlite3_bind_int(stmt, 1, alarm->rule_id);
    sqlite3_bind_int(stmt, 2, alarm->module_id);
    sqlite3_bind_int(stmt, 3, (int)alarm->severity);
    sqlite3_bind_text(stmt, 4, alarm->message, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 5, alarm->trigger_value);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) return RESULT_ERROR;
    
    *alarm_id = (int)sqlite3_last_insert_rowid(db->db);
    alarm->id = *alarm_id;
    alarm->state = ALARM_STATE_ACTIVE;
    
    LOG_WARNING("Alarm raised: %s (id=%d, severity=%d)", alarm->message, *alarm_id, alarm->severity);
    return RESULT_OK;
}

result_t db_alarm_acknowledge(database_t *db, int alarm_id, const char *acknowledged_by) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "UPDATE alarm_history SET state='acknowledged', acknowledged_time=datetime('now'), acknowledged_by=? WHERE id=? AND state='active';";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_text(stmt, 1, acknowledged_by ? acknowledged_by : "operator", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, alarm_id);
    
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db->db);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE && changes > 0) {
        LOG_INFO("Alarm %d acknowledged by %s", alarm_id, acknowledged_by ? acknowledged_by : "operator");
        return RESULT_OK;
    }
    return RESULT_NOT_FOUND;
}

result_t db_alarm_clear(database_t *db, int alarm_id) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "UPDATE alarm_history SET state='cleared', cleared_time=datetime('now') WHERE id=? AND state IN ('active', 'acknowledged');";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, alarm_id);
    
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db->db);
    sqlite3_finalize(stmt);
    
    if (rc == SQLITE_DONE && changes > 0) {
        LOG_INFO("Alarm %d cleared", alarm_id);
        return RESULT_OK;
    }
    return RESULT_NOT_FOUND;
}

result_t db_alarm_clear_by_rule(database_t *db, int rule_id) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "UPDATE alarm_history SET state='cleared', cleared_time=datetime('now') WHERE rule_id=? AND state IN ('active', 'acknowledged');";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, rule_id);
    
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db->db);
    sqlite3_finalize(stmt);
    
    if (changes > 0) LOG_INFO("Cleared %d alarms for rule %d", changes, rule_id);
    return rc == SQLITE_DONE ? RESULT_OK : RESULT_ERROR;
}

result_t db_alarm_get(database_t *db, int alarm_id, db_alarm_history_t *alarm) {
    CHECK_NULL(db); CHECK_NULL(alarm);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT id, rule_id, module_id, severity, state, message, trigger_value, strftime('%s', raised_time), strftime('%s', acknowledged_time), strftime('%s', cleared_time), acknowledged_by FROM alarm_history WHERE id=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, alarm_id);
    
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return RESULT_NOT_FOUND;
    }
    
    alarm->id = sqlite3_column_int(stmt, 0);
    alarm->rule_id = sqlite3_column_int(stmt, 1);
    alarm->module_id = sqlite3_column_int(stmt, 2);
    alarm->severity = (alarm_severity_t)sqlite3_column_int(stmt, 3);
    
    const char *state_str = (const char*)sqlite3_column_text(stmt, 4);
    if (strcmp(state_str, "active") == 0) alarm->state = ALARM_STATE_ACTIVE;
    else if (strcmp(state_str, "acknowledged") == 0) alarm->state = ALARM_STATE_ACKNOWLEDGED;
    else alarm->state = ALARM_STATE_CLEARED;
    
    SAFE_STRNCPY(alarm->message, (const char*)sqlite3_column_text(stmt, 5), sizeof(alarm->message));
    alarm->trigger_value = sqlite3_column_double(stmt, 6);
    alarm->raised_time = (time_t)sqlite3_column_int64(stmt, 7);
    alarm->acknowledged_time = (time_t)sqlite3_column_int64(stmt, 8);
    alarm->cleared_time = (time_t)sqlite3_column_int64(stmt, 9);
    SAFE_STRNCPY(alarm->acknowledged_by, (const char*)sqlite3_column_text(stmt, 10), sizeof(alarm->acknowledged_by));
    
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_alarm_list_active(database_t *db, db_alarm_history_t **alarms, int *count) {
    CHECK_NULL(db); CHECK_NULL(alarms); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    *alarms = NULL;
    *count = 0;
    
    const char *sql = "SELECT id, rule_id, module_id, severity, state, message, trigger_value, strftime('%s', raised_time), strftime('%s', acknowledged_time), strftime('%s', cleared_time), acknowledged_by FROM alarm_history WHERE state IN ('active', 'acknowledged') ORDER BY severity DESC, raised_time DESC;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) row_count++;
    sqlite3_reset(stmt);
    
    if (row_count == 0) {
        sqlite3_finalize(stmt);
        return RESULT_OK;
    }
    
    *alarms = calloc(row_count, sizeof(db_alarm_history_t));
    if (!*alarms) {
        sqlite3_finalize(stmt);
        return RESULT_NO_MEMORY;
    }
    
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < row_count) {
        (*alarms)[idx].id = sqlite3_column_int(stmt, 0);
        (*alarms)[idx].rule_id = sqlite3_column_int(stmt, 1);
        (*alarms)[idx].module_id = sqlite3_column_int(stmt, 2);
        (*alarms)[idx].severity = (alarm_severity_t)sqlite3_column_int(stmt, 3);
        
        const char *state_str = (const char*)sqlite3_column_text(stmt, 4);
        if (strcmp(state_str, "active") == 0) (*alarms)[idx].state = ALARM_STATE_ACTIVE;
        else (*alarms)[idx].state = ALARM_STATE_ACKNOWLEDGED;
        
        SAFE_STRNCPY((*alarms)[idx].message, (const char*)sqlite3_column_text(stmt, 5), sizeof((*alarms)[idx].message));
        (*alarms)[idx].trigger_value = sqlite3_column_double(stmt, 6);
        (*alarms)[idx].raised_time = (time_t)sqlite3_column_int64(stmt, 7);
        (*alarms)[idx].acknowledged_time = (time_t)sqlite3_column_int64(stmt, 8);
        (*alarms)[idx].cleared_time = (time_t)sqlite3_column_int64(stmt, 9);
        SAFE_STRNCPY((*alarms)[idx].acknowledged_by, (const char*)sqlite3_column_text(stmt, 10), sizeof((*alarms)[idx].acknowledged_by));
        idx++;
    }
    
    sqlite3_finalize(stmt);
    *count = idx;
    return RESULT_OK;
}

result_t db_alarm_count_active(database_t *db, int *count) {
    CHECK_NULL(db); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT COUNT(*) FROM alarm_history WHERE state IN ('active', 'acknowledged');";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    *count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) *count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_alarm_count_by_severity(database_t *db, alarm_severity_t severity, int *count) {
    CHECK_NULL(db); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT COUNT(*) FROM alarm_history WHERE state IN ('active', 'acknowledged') AND severity=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, (int)severity);
    
    *count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) *count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_alarm_has_active_for_rule(database_t *db, int rule_id, bool *has_active) {
    CHECK_NULL(db); CHECK_NULL(has_active);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT COUNT(*) FROM alarm_history WHERE rule_id=? AND state IN ('active', 'acknowledged');";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_int(stmt, 1, rule_id);
    
    *has_active = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) *has_active = sqlite3_column_int(stmt, 0) > 0;
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_alarm_cleanup(database_t *db, int retention_days) {
    CHECK_NULL(db);
    if (!db->db || retention_days <= 0) return RESULT_INVALID_PARAM;
    
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM alarm_history WHERE state='cleared' AND cleared_time < datetime('now', '-%d days');", retention_days);
    
    char *err = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &err);
    
    if (rc != SQLITE_OK) {
        LOG_ERROR("Alarm cleanup failed: %s", err);
        sqlite3_free(err);
        return RESULT_ERROR;
    }
    
    int deleted = sqlite3_changes(db->db);
    if (deleted > 0) LOG_INFO("Cleaned up %d old alarms", deleted);
    return RESULT_OK;
}

void db_alarm_free_list(db_alarm_rule_t *rules) { if (rules) free(rules); }
void db_alarm_history_free_list(db_alarm_history_t *alarms) { if (alarms) free(alarms); }

const char* alarm_severity_to_string(alarm_severity_t severity) {
    switch (severity) {
        case ALARM_SEVERITY_LOW: return "Low";
        case ALARM_SEVERITY_MEDIUM: return "Medium";
        case ALARM_SEVERITY_HIGH: return "High";
        case ALARM_SEVERITY_CRITICAL: return "Critical";
        default: return "Unknown";
    }
}

const char* alarm_condition_to_string(alarm_condition_t condition) {
    switch (condition) {
        case ALARM_CONDITION_ABOVE_THRESHOLD: return "Above Threshold";
        case ALARM_CONDITION_BELOW_THRESHOLD: return "Below Threshold";
        case ALARM_CONDITION_OUT_OF_RANGE: return "Out of Range";
        case ALARM_CONDITION_RATE_OF_CHANGE: return "Rate of Change";
        default: return "Unknown";
    }
}

const char* alarm_state_to_string(alarm_state_t state) {
    switch (state) {
        case ALARM_STATE_CLEARED: return "Cleared";
        case ALARM_STATE_ACTIVE: return "Active";
        case ALARM_STATE_ACKNOWLEDGED: return "Acknowledged";
        default: return "Unknown";
    }
}
