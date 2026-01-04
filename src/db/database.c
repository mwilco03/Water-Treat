#include "database.h"
#include "utils/logger.h"

/* Schema split into individual statements to avoid overlength string literals */
static const char *SCHEMA_STATEMENTS[] = {
    "CREATE TABLE IF NOT EXISTS modules (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "slot INTEGER NOT NULL UNIQUE, subslot INTEGER DEFAULT 0, name TEXT NOT NULL, "
    "module_type TEXT NOT NULL, module_ident INTEGER, submodule_ident INTEGER, "
    "status TEXT DEFAULT 'inactive', created_at DATETIME DEFAULT CURRENT_TIMESTAMP, "
    "updated_at DATETIME DEFAULT CURRENT_TIMESTAMP)",

    "CREATE TABLE IF NOT EXISTS physical_sensors (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "module_id INTEGER NOT NULL UNIQUE, sensor_type TEXT NOT NULL, hardware_type TEXT, "
    "interface TEXT NOT NULL, address TEXT, bus INTEGER DEFAULT 0, channel INTEGER DEFAULT 0, "
    "resolution REAL DEFAULT 0.1, unit TEXT, min_value REAL, max_value REAL, "
    "poll_rate_ms INTEGER DEFAULT 1000, timeout_ms INTEGER DEFAULT 5000, "
    "FOREIGN KEY (module_id) REFERENCES modules(id) ON DELETE CASCADE)",

    "CREATE TABLE IF NOT EXISTS adc_sensors (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "module_id INTEGER NOT NULL UNIQUE, adc_type TEXT NOT NULL, interface TEXT NOT NULL, "
    "address TEXT, bus INTEGER DEFAULT 0, channel INTEGER NOT NULL, gain INTEGER DEFAULT 1, "
    "reference_voltage REAL DEFAULT 3.3, unit TEXT, raw_min INTEGER DEFAULT 0, "
    "raw_max INTEGER DEFAULT 65535, eng_min REAL DEFAULT 0, eng_max REAL DEFAULT 100, "
    "poll_rate_ms INTEGER DEFAULT 1000, FOREIGN KEY (module_id) REFERENCES modules(id) ON DELETE CASCADE)",

    "CREATE TABLE IF NOT EXISTS web_poll_sensors (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "module_id INTEGER NOT NULL UNIQUE, url TEXT NOT NULL, method TEXT DEFAULT 'GET', "
    "headers TEXT, json_path TEXT, poll_rate_ms INTEGER DEFAULT 60000, timeout_ms INTEGER DEFAULT 10000, "
    "FOREIGN KEY (module_id) REFERENCES modules(id) ON DELETE CASCADE)",

    "CREATE TABLE IF NOT EXISTS calculated_sensors (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "module_id INTEGER NOT NULL UNIQUE, formula TEXT NOT NULL, input_sensors TEXT, unit TEXT, "
    "update_rate_ms INTEGER DEFAULT 1000, FOREIGN KEY (module_id) REFERENCES modules(id) ON DELETE CASCADE)",

    "CREATE TABLE IF NOT EXISTS static_sensors (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "module_id INTEGER NOT NULL UNIQUE, value REAL NOT NULL, unit TEXT, writable INTEGER DEFAULT 0, "
    "FOREIGN KEY (module_id) REFERENCES modules(id) ON DELETE CASCADE)",

    "CREATE TABLE IF NOT EXISTS sensor_status (module_id INTEGER PRIMARY KEY, value REAL, "
    "status TEXT DEFAULT 'unknown', last_update DATETIME DEFAULT CURRENT_TIMESTAMP, "
    "consecutive_failures INTEGER DEFAULT 0, FOREIGN KEY (module_id) REFERENCES modules(id) ON DELETE CASCADE)",

    "CREATE TABLE IF NOT EXISTS sensor_data_log (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "module_id INTEGER NOT NULL, value REAL NOT NULL, status TEXT DEFAULT 'ok', "
    "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, FOREIGN KEY (module_id) REFERENCES modules(id) ON DELETE CASCADE)",

    "CREATE INDEX IF NOT EXISTS idx_sensor_log_time ON sensor_data_log(module_id, timestamp)",

    "CREATE TABLE IF NOT EXISTS alarm_rules (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "module_id INTEGER NOT NULL, name TEXT, condition INTEGER NOT NULL, threshold_high REAL, "
    "threshold_low REAL, severity INTEGER DEFAULT 2, enabled INTEGER DEFAULT 1, auto_clear INTEGER DEFAULT 1, "
    "hysteresis_percent INTEGER DEFAULT 5, interlock_enabled INTEGER DEFAULT 0, interlock_slot INTEGER DEFAULT 0, "
    "interlock_action INTEGER DEFAULT 0, interlock_pwm_duty INTEGER DEFAULT 0, release_on_clear INTEGER DEFAULT 1, "
    "FOREIGN KEY (module_id) REFERENCES modules(id) ON DELETE CASCADE)",

    "CREATE INDEX IF NOT EXISTS idx_alarm_rules_module ON alarm_rules(module_id)",

    "CREATE TABLE IF NOT EXISTS alarm_history (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "rule_id INTEGER, module_id INTEGER NOT NULL, severity INTEGER NOT NULL, "
    "state TEXT NOT NULL DEFAULT 'active', message TEXT, trigger_value REAL, "
    "raised_time DATETIME DEFAULT CURRENT_TIMESTAMP, acknowledged_time DATETIME, "
    "cleared_time DATETIME, acknowledged_by TEXT, FOREIGN KEY (module_id) REFERENCES modules(id) ON DELETE CASCADE)",

    "CREATE INDEX IF NOT EXISTS idx_alarm_state ON alarm_history(state)",

    "CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, source TEXT, level TEXT DEFAULT 'info', message TEXT)",

    "CREATE TABLE IF NOT EXISTS logging_config (id INTEGER PRIMARY KEY CHECK (id = 1), "
    "enabled INTEGER DEFAULT 0, interval_seconds INTEGER DEFAULT 60, retention_days INTEGER DEFAULT 30, "
    "remote_url TEXT, remote_enabled INTEGER DEFAULT 0)",

    "INSERT OR IGNORE INTO logging_config (id) VALUES (1)",

    "CREATE TABLE IF NOT EXISTS actuators (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "slot INTEGER NOT NULL UNIQUE, subslot INTEGER DEFAULT 0, name TEXT NOT NULL, "
    "type TEXT DEFAULT 'relay', gpio_pin INTEGER NOT NULL, gpio_chip TEXT DEFAULT 'gpiochip0', "
    "active_low INTEGER DEFAULT 0, safe_state TEXT DEFAULT 'hold', min_on_time_ms INTEGER DEFAULT 0, "
    "max_on_time_ms INTEGER DEFAULT 0, pwm_frequency_hz INTEGER DEFAULT 1000, "
    "status TEXT DEFAULT 'inactive', enabled INTEGER DEFAULT 1, "
    "created_at DATETIME DEFAULT CURRENT_TIMESTAMP, updated_at DATETIME DEFAULT CURRENT_TIMESTAMP)",

    "CREATE TABLE IF NOT EXISTS actuator_state (actuator_id INTEGER PRIMARY KEY, "
    "state INTEGER DEFAULT 0, pwm_duty INTEGER DEFAULT 0, "
    "last_state_change DATETIME DEFAULT CURRENT_TIMESTAMP, "
    "total_on_time_ms INTEGER DEFAULT 0, cycle_count INTEGER DEFAULT 0, "
    "FOREIGN KEY (actuator_id) REFERENCES actuators(id) ON DELETE CASCADE)",

    NULL  /* Sentinel */
};

result_t database_init(database_t *db, const char *path) {
    CHECK_NULL(db); CHECK_NULL(path);
    memset(db,0,sizeof(*db)); SAFE_STRNCPY(db->db_path,path,sizeof(db->db_path));
    int rc = sqlite3_open(path, &db->db);
    if (rc != SQLITE_OK) { LOG_ERROR("Failed to open database: %s", sqlite3_errmsg(db->db)); sqlite3_close(db->db); db->db=NULL; return RESULT_IO_ERROR; }
    sqlite3_exec(db->db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    sqlite3_busy_timeout(db->db, 5000);
    sqlite3_exec(db->db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
    /* Execute each schema statement */
    for (int i = 0; SCHEMA_STATEMENTS[i] != NULL; i++) {
        char *err = NULL;
        rc = sqlite3_exec(db->db, SCHEMA_STATEMENTS[i], NULL, NULL, &err);
        if (rc != SQLITE_OK) { LOG_ERROR("Schema error: %s", err); sqlite3_free(err); sqlite3_close(db->db); db->db=NULL; return RESULT_ERROR; }
    }
    db->initialized = true; LOG_INFO("Database initialized: %s", path); return RESULT_OK;
}

void database_close(database_t *db) { if (db && db->db) { sqlite3_close(db->db); db->db=NULL; db->initialized=false; LOG_INFO("Database closed"); } }
bool database_is_connected(database_t *db) { return db && db->db && db->initialized; }
result_t database_execute(database_t *db, const char *sql) { CHECK_NULL(db); CHECK_NULL(sql); if (!db->db) return RESULT_NOT_INITIALIZED; char *err=NULL; int rc=sqlite3_exec(db->db,sql,NULL,NULL,&err); if(rc!=SQLITE_OK) { LOG_ERROR("SQL: %s",err); sqlite3_free(err); return RESULT_ERROR; } return RESULT_OK; }
result_t database_begin_transaction(database_t *db) { return database_execute(db, "BEGIN TRANSACTION;"); }
result_t database_commit(database_t *db) { return database_execute(db, "COMMIT;"); }
result_t database_rollback(database_t *db) { return database_execute(db, "ROLLBACK;"); }
int64_t database_last_insert_id(database_t *db) { return db && db->db ? sqlite3_last_insert_rowid(db->db) : 0; }
int database_changes(database_t *db) { return db && db->db ? sqlite3_changes(db->db) : 0; }
const char* database_error_message(database_t *db) { return db && db->db ? sqlite3_errmsg(db->db) : "Not initialized"; }
