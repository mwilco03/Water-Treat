/**
 * @file db_events.c
 * @brief Event logging database operations
 */

#include "db_events.h"
#include "utils/logger.h"

result_t db_event_insert(database_t *db, const char *source, const char *level, const char *message) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "INSERT INTO events (source, level, message) VALUES (?, ?, ?);";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("Failed to prepare event insert: %s", sqlite3_errmsg(db->db));
        return RESULT_ERROR;
    }
    
    sqlite3_bind_text(stmt, 1, source ? source : "system", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, level ? level : "info", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, message ? message : "", -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return rc == SQLITE_DONE ? RESULT_OK : RESULT_ERROR;
}

result_t db_event_insert_formatted(database_t *db, const char *source, const char *level, const char *fmt, ...) {
    CHECK_NULL(db); CHECK_NULL(fmt);
    
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    
    return db_event_insert(db, source, level, message);
}

result_t db_event_list(database_t *db, int limit, db_event_t **events, int *count) {
    CHECK_NULL(db); CHECK_NULL(events); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    *events = NULL;
    *count = 0;
    
    if (limit <= 0) limit = 100;
    if (limit > 1000) limit = 1000;
    
    // Count first
    const char *count_sql = "SELECT COUNT(*) FROM events;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, count_sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    int total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    
    if (total == 0) return RESULT_OK;
    
    int fetch_count = MIN(total, limit);
    *events = calloc(fetch_count, sizeof(db_event_t));
    if (!*events) return RESULT_NO_MEMORY;
    
    const char *sql = "SELECT id, strftime('%s', timestamp), source, level, message FROM events ORDER BY timestamp DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(*events);
        *events = NULL;
        return RESULT_ERROR;
    }
    
    sqlite3_bind_int(stmt, 1, limit);
    
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < fetch_count) {
        (*events)[idx].id = sqlite3_column_int(stmt, 0);
        (*events)[idx].timestamp = (time_t)sqlite3_column_int64(stmt, 1);
        SAFE_STRNCPY((*events)[idx].source, (const char*)sqlite3_column_text(stmt, 2), sizeof((*events)[idx].source));
        SAFE_STRNCPY((*events)[idx].level, (const char*)sqlite3_column_text(stmt, 3), sizeof((*events)[idx].level));
        SAFE_STRNCPY((*events)[idx].message, (const char*)sqlite3_column_text(stmt, 4), sizeof((*events)[idx].message));
        idx++;
    }
    
    sqlite3_finalize(stmt);
    *count = idx;
    return RESULT_OK;
}

result_t db_event_list_by_source(database_t *db, const char *source, int limit, db_event_t **events, int *count) {
    CHECK_NULL(db); CHECK_NULL(source); CHECK_NULL(events); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    *events = NULL;
    *count = 0;
    
    if (limit <= 0) limit = 100;
    
    const char *sql = "SELECT id, strftime('%s', timestamp), source, level, message FROM events WHERE source=? ORDER BY timestamp DESC LIMIT ?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_text(stmt, 1, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    
    // Count results first
    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) row_count++;
    sqlite3_reset(stmt);
    
    if (row_count == 0) {
        sqlite3_finalize(stmt);
        return RESULT_OK;
    }
    
    *events = calloc(row_count, sizeof(db_event_t));
    if (!*events) {
        sqlite3_finalize(stmt);
        return RESULT_NO_MEMORY;
    }
    
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < row_count) {
        (*events)[idx].id = sqlite3_column_int(stmt, 0);
        (*events)[idx].timestamp = (time_t)sqlite3_column_int64(stmt, 1);
        SAFE_STRNCPY((*events)[idx].source, (const char*)sqlite3_column_text(stmt, 2), sizeof((*events)[idx].source));
        SAFE_STRNCPY((*events)[idx].level, (const char*)sqlite3_column_text(stmt, 3), sizeof((*events)[idx].level));
        SAFE_STRNCPY((*events)[idx].message, (const char*)sqlite3_column_text(stmt, 4), sizeof((*events)[idx].message));
        idx++;
    }
    
    sqlite3_finalize(stmt);
    *count = idx;
    return RESULT_OK;
}

result_t db_event_list_by_level(database_t *db, const char *level, int limit, db_event_t **events, int *count) {
    CHECK_NULL(db); CHECK_NULL(level); CHECK_NULL(events); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    *events = NULL;
    *count = 0;
    
    if (limit <= 0) limit = 100;
    
    const char *sql = "SELECT id, strftime('%s', timestamp), source, level, message FROM events WHERE level=? ORDER BY timestamp DESC LIMIT ?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_text(stmt, 1, level, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    
    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) row_count++;
    sqlite3_reset(stmt);
    
    if (row_count == 0) {
        sqlite3_finalize(stmt);
        return RESULT_OK;
    }
    
    *events = calloc(row_count, sizeof(db_event_t));
    if (!*events) {
        sqlite3_finalize(stmt);
        return RESULT_NO_MEMORY;
    }
    
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < row_count) {
        (*events)[idx].id = sqlite3_column_int(stmt, 0);
        (*events)[idx].timestamp = (time_t)sqlite3_column_int64(stmt, 1);
        SAFE_STRNCPY((*events)[idx].source, (const char*)sqlite3_column_text(stmt, 2), sizeof((*events)[idx].source));
        SAFE_STRNCPY((*events)[idx].level, (const char*)sqlite3_column_text(stmt, 3), sizeof((*events)[idx].level));
        SAFE_STRNCPY((*events)[idx].message, (const char*)sqlite3_column_text(stmt, 4), sizeof((*events)[idx].message));
        idx++;
    }
    
    sqlite3_finalize(stmt);
    *count = idx;
    return RESULT_OK;
}

result_t db_event_list_recent(database_t *db, int minutes, db_event_t **events, int *count) {
    CHECK_NULL(db); CHECK_NULL(events); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    *events = NULL;
    *count = 0;
    
    if (minutes <= 0) minutes = 60;
    
    char sql[256];
    snprintf(sql, sizeof(sql), 
        "SELECT id, strftime('%%s', timestamp), source, level, message FROM events "
        "WHERE timestamp >= datetime('now', '-%d minutes') ORDER BY timestamp DESC;", minutes);
    
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) row_count++;
    sqlite3_reset(stmt);
    
    if (row_count == 0) {
        sqlite3_finalize(stmt);
        return RESULT_OK;
    }
    
    *events = calloc(row_count, sizeof(db_event_t));
    if (!*events) {
        sqlite3_finalize(stmt);
        return RESULT_NO_MEMORY;
    }
    
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < row_count) {
        (*events)[idx].id = sqlite3_column_int(stmt, 0);
        (*events)[idx].timestamp = (time_t)sqlite3_column_int64(stmt, 1);
        SAFE_STRNCPY((*events)[idx].source, (const char*)sqlite3_column_text(stmt, 2), sizeof((*events)[idx].source));
        SAFE_STRNCPY((*events)[idx].level, (const char*)sqlite3_column_text(stmt, 3), sizeof((*events)[idx].level));
        SAFE_STRNCPY((*events)[idx].message, (const char*)sqlite3_column_text(stmt, 4), sizeof((*events)[idx].message));
        idx++;
    }
    
    sqlite3_finalize(stmt);
    *count = idx;
    return RESULT_OK;
}

result_t db_event_cleanup(database_t *db, int retention_days) {
    CHECK_NULL(db);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    if (retention_days <= 0) return RESULT_OK;
    
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM events WHERE timestamp < datetime('now', '-%d days');", retention_days);
    
    char *err = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &err);
    
    if (rc != SQLITE_OK) {
        LOG_ERROR("Event cleanup failed: %s", err);
        sqlite3_free(err);
        return RESULT_ERROR;
    }
    
    int deleted = sqlite3_changes(db->db);
    if (deleted > 0) LOG_INFO("Cleaned up %d old events", deleted);
    return RESULT_OK;
}

result_t db_event_count(database_t *db, int *count) {
    CHECK_NULL(db); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT COUNT(*) FROM events;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    
    *count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) *count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

result_t db_event_count_by_level(database_t *db, const char *level, int *count) {
    CHECK_NULL(db); CHECK_NULL(level); CHECK_NULL(count);
    if (!db->db) return RESULT_NOT_INITIALIZED;
    
    const char *sql = "SELECT COUNT(*) FROM events WHERE level=?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return RESULT_ERROR;
    sqlite3_bind_text(stmt, 1, level, -1, SQLITE_TRANSIENT);
    
    *count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) *count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

void db_event_free_list(db_event_t *events) {
    if (events) free(events);
}
