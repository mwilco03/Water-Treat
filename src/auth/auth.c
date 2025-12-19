/**
 * @file auth.c
 * @brief Authentication and session management implementation
 */

#include "auth.h"
#include "utils/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Global session state */
auth_session_t g_auth_session = {0};

/* Default credentials - Water treatment puns! */
#define DEFAULT_USERNAME    "admin"
#define DEFAULT_PASSWORD    "H2OhYeah!"  /* H2O + Oh Yeah! */
#define DEFAULT_SALT        "NaCl4Life"  /* Salt for hashing, also a chemistry pun */

/* Alternative fun passwords for reference:
 * "Cl3@nW@ter"     - Clean Water with l33t
 * "pH7.0Perfect"   - Neutral pH
 * "FlowWithIt!"    - Flow reference
 * "TurbidityZero"  - Clear water
 * "DrinkM3In!"     - Drink Me In
 */

/* ============================================================================
 * Password Hashing (Simple DJB2 + Salt for embedded use)
 * ========================================================================== */

static unsigned long djb2_hash(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

void auth_hash_password(const char *password, const char *salt, char *hash_out) {
    char combined[256];
    snprintf(combined, sizeof(combined), "%s:%s:%s", salt, password, salt);

    unsigned long hash1 = djb2_hash(combined);

    /* Double hash for slight extra security */
    char temp[64];
    snprintf(temp, sizeof(temp), "%lu:%s", hash1, salt);
    unsigned long hash2 = djb2_hash(temp);

    snprintf(hash_out, AUTH_MAX_HASH, "%016lx%016lx", hash1, hash2);
}

static bool verify_password(const char *password, const char *stored_hash) {
    char computed_hash[AUTH_MAX_HASH];
    auth_hash_password(password, DEFAULT_SALT, computed_hash);
    return (strcmp(computed_hash, stored_hash) == 0);
}

/* ============================================================================
 * Database Operations
 * ========================================================================== */

static result_t ensure_users_table(database_t *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT UNIQUE NOT NULL,"
        "  password_hash TEXT NOT NULL,"
        "  role INTEGER DEFAULT 1,"
        "  enabled INTEGER DEFAULT 1,"
        "  created_at INTEGER,"
        "  last_login INTEGER,"
        "  login_failures INTEGER DEFAULT 0"
        ");";

    char *err = NULL;
    if (sqlite3_exec(db->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOG_ERROR("Failed to create users table: %s", err);
        sqlite3_free(err);
        return RESULT_ERROR;
    }
    return RESULT_OK;
}

static int count_users(database_t *db) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT COUNT(*) FROM users;";

    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

static result_t create_default_admin(database_t *db) {
    char hash[AUTH_MAX_HASH];
    auth_hash_password(DEFAULT_PASSWORD, DEFAULT_SALT, hash);

    const char *sql =
        "INSERT INTO users (username, password_hash, role, enabled, created_at) "
        "VALUES (?, ?, ?, 1, ?);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("Failed to prepare default admin insert");
        return RESULT_ERROR;
    }

    sqlite3_bind_text(stmt, 1, DEFAULT_USERNAME, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, AUTH_ROLE_ADMIN);
    sqlite3_bind_int64(stmt, 4, time(NULL));

    result_t result = RESULT_OK;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        LOG_ERROR("Failed to create default admin user");
        result = RESULT_ERROR;
    } else {
        LOG_INFO("Created default admin user: %s / %s", DEFAULT_USERNAME, DEFAULT_PASSWORD);
        LOG_INFO("  (Hint: H2O + Oh Yeah! = %s)", DEFAULT_PASSWORD);
    }

    sqlite3_finalize(stmt);
    return result;
}

/* ============================================================================
 * Authentication API Implementation
 * ========================================================================== */

result_t auth_init(database_t *db) {
    CHECK_NULL(db);

    /* Ensure users table exists */
    if (ensure_users_table(db) != RESULT_OK) {
        return RESULT_ERROR;
    }

    /* Create default admin if no users exist */
    int user_count = count_users(db);
    if (user_count == 0) {
        LOG_INFO("No users found, creating default admin account...");
        if (create_default_admin(db) != RESULT_OK) {
            return RESULT_ERROR;
        }
    }

    /* Clear any existing session */
    memset(&g_auth_session, 0, sizeof(g_auth_session));

    LOG_INFO("Authentication system initialized (%d user(s))",
             user_count > 0 ? user_count : 1);
    return RESULT_OK;
}

result_t auth_login(database_t *db, const char *username, const char *password) {
    CHECK_NULL(db);
    CHECK_NULL(username);
    CHECK_NULL(password);

    const char *sql =
        "SELECT id, username, password_hash, role, enabled, created_at, "
        "       last_login, login_failures "
        "FROM users WHERE username = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return RESULT_ERROR;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    result_t result = RESULT_NOT_FOUND;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        auth_user_t user = {0};
        user.id = sqlite3_column_int(stmt, 0);
        SAFE_STRNCPY(user.username, (const char*)sqlite3_column_text(stmt, 1),
                     sizeof(user.username));
        SAFE_STRNCPY(user.password_hash, (const char*)sqlite3_column_text(stmt, 2),
                     sizeof(user.password_hash));
        user.role = sqlite3_column_int(stmt, 3);
        user.enabled = sqlite3_column_int(stmt, 4);
        user.created_at = sqlite3_column_int64(stmt, 5);
        user.last_login = sqlite3_column_int64(stmt, 6);
        user.login_failures = sqlite3_column_int(stmt, 7);

        if (!user.enabled) {
            LOG_WARNING("Login attempt for disabled user: %s", username);
            result = RESULT_ERROR;
        } else if (verify_password(password, user.password_hash)) {
            /* Success! */
            g_auth_session.authenticated = true;
            g_auth_session.user = user;
            g_auth_session.login_time = time(NULL);
            g_auth_session.last_activity = g_auth_session.login_time;

            /* Update last_login and reset failures */
            const char *update_sql =
                "UPDATE users SET last_login = ?, login_failures = 0 WHERE id = ?;";
            sqlite3_stmt *update_stmt;
            if (sqlite3_prepare_v2(db->db, update_sql, -1, &update_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(update_stmt, 1, g_auth_session.login_time);
                sqlite3_bind_int(update_stmt, 2, user.id);
                sqlite3_step(update_stmt);
                sqlite3_finalize(update_stmt);
            }

            LOG_INFO("User '%s' logged in successfully (role: %s)",
                     username, auth_role_to_string(user.role));
            result = RESULT_OK;
        } else {
            /* Wrong password - increment failure count */
            const char *fail_sql =
                "UPDATE users SET login_failures = login_failures + 1 WHERE id = ?;";
            sqlite3_stmt *fail_stmt;
            if (sqlite3_prepare_v2(db->db, fail_sql, -1, &fail_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int(fail_stmt, 1, user.id);
                sqlite3_step(fail_stmt);
                sqlite3_finalize(fail_stmt);
            }

            LOG_WARNING("Failed login attempt for user: %s", username);
            result = RESULT_ERROR;
        }
    } else {
        LOG_WARNING("Login attempt for unknown user: %s", username);
    }

    sqlite3_finalize(stmt);
    return result;
}

void auth_logout(void) {
    if (g_auth_session.authenticated) {
        LOG_INFO("User '%s' logged out", g_auth_session.user.username);
    }
    memset(&g_auth_session, 0, sizeof(g_auth_session));
}

bool auth_is_logged_in(void) {
    return g_auth_session.authenticated;
}

bool auth_has_role(auth_role_t required_role) {
    if (!g_auth_session.authenticated) {
        return false;
    }
    return g_auth_session.user.role >= required_role;
}

const char* auth_get_username(void) {
    if (g_auth_session.authenticated) {
        return g_auth_session.user.username;
    }
    return "anonymous";
}

void auth_touch_session(void) {
    if (g_auth_session.authenticated) {
        g_auth_session.last_activity = time(NULL);
    }
}

bool auth_check_timeout(void) {
    if (!g_auth_session.authenticated) {
        return false;
    }

    time_t now = time(NULL);
    if (now - g_auth_session.last_activity > AUTH_SESSION_TIMEOUT) {
        LOG_INFO("Session timeout for user '%s'", g_auth_session.user.username);
        auth_logout();
        return true;
    }
    return false;
}

/* ============================================================================
 * User Management API
 * ========================================================================== */

result_t auth_user_create(database_t *db, const char *username,
                          const char *password, auth_role_t role) {
    CHECK_NULL(db);
    CHECK_NULL(username);
    CHECK_NULL(password);

    char hash[AUTH_MAX_HASH];
    auth_hash_password(password, DEFAULT_SALT, hash);

    const char *sql =
        "INSERT INTO users (username, password_hash, role, enabled, created_at) "
        "VALUES (?, ?, ?, 1, ?);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return RESULT_ERROR;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, role);
    sqlite3_bind_int64(stmt, 4, time(NULL));

    result_t result = RESULT_OK;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        if (sqlite3_errcode(db->db) == SQLITE_CONSTRAINT) {
            result = RESULT_ALREADY_EXISTS;
        } else {
            result = RESULT_ERROR;
        }
    } else {
        LOG_INFO("Created user '%s' with role %s", username, auth_role_to_string(role));
    }

    sqlite3_finalize(stmt);
    return result;
}

result_t auth_user_change_password(database_t *db, int user_id,
                                   const char *new_password) {
    CHECK_NULL(db);
    CHECK_NULL(new_password);

    char hash[AUTH_MAX_HASH];
    auth_hash_password(new_password, DEFAULT_SALT, hash);

    const char *sql = "UPDATE users SET password_hash = ? WHERE id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return RESULT_ERROR;
    }

    sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, user_id);

    result_t result = RESULT_OK;
    if (sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(db->db) == 0) {
        result = RESULT_NOT_FOUND;
    } else {
        LOG_INFO("Password changed for user ID %d", user_id);
    }

    sqlite3_finalize(stmt);
    return result;
}

result_t auth_user_delete(database_t *db, int user_id) {
    CHECK_NULL(db);

    /* Don't allow deleting the last admin */
    const char *count_sql = "SELECT COUNT(*) FROM users WHERE role = ?;";
    sqlite3_stmt *count_stmt;
    if (sqlite3_prepare_v2(db->db, count_sql, -1, &count_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(count_stmt, 1, AUTH_ROLE_ADMIN);
        if (sqlite3_step(count_stmt) == SQLITE_ROW) {
            int admin_count = sqlite3_column_int(count_stmt, 0);
            sqlite3_finalize(count_stmt);

            /* Check if this user is an admin */
            const char *role_sql = "SELECT role FROM users WHERE id = ?;";
            sqlite3_stmt *role_stmt;
            if (sqlite3_prepare_v2(db->db, role_sql, -1, &role_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int(role_stmt, 1, user_id);
                if (sqlite3_step(role_stmt) == SQLITE_ROW) {
                    int role = sqlite3_column_int(role_stmt, 0);
                    if (role == AUTH_ROLE_ADMIN && admin_count <= 1) {
                        sqlite3_finalize(role_stmt);
                        LOG_WARNING("Cannot delete last admin user");
                        return RESULT_ERROR;
                    }
                }
                sqlite3_finalize(role_stmt);
            }
        } else {
            sqlite3_finalize(count_stmt);
        }
    }

    const char *sql = "DELETE FROM users WHERE id = ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return RESULT_ERROR;
    }

    sqlite3_bind_int(stmt, 1, user_id);

    result_t result = RESULT_OK;
    if (sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(db->db) == 0) {
        result = RESULT_NOT_FOUND;
    } else {
        LOG_INFO("Deleted user ID %d", user_id);
    }

    sqlite3_finalize(stmt);
    return result;
}

result_t auth_user_list(database_t *db, auth_user_t **users, int *count) {
    CHECK_NULL(db);
    CHECK_NULL(users);
    CHECK_NULL(count);

    *users = NULL;
    *count = 0;

    /* Count users first */
    int total = count_users(db);
    if (total <= 0) {
        return total == 0 ? RESULT_OK : RESULT_ERROR;
    }

    *users = calloc(total, sizeof(auth_user_t));
    if (!*users) {
        return RESULT_NO_MEMORY;
    }

    const char *sql =
        "SELECT id, username, password_hash, role, enabled, created_at, "
        "       last_login, login_failures FROM users ORDER BY id;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(*users);
        *users = NULL;
        return RESULT_ERROR;
    }

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < total) {
        auth_user_t *u = &(*users)[idx];
        u->id = sqlite3_column_int(stmt, 0);
        SAFE_STRNCPY(u->username, (const char*)sqlite3_column_text(stmt, 1),
                     sizeof(u->username));
        SAFE_STRNCPY(u->password_hash, (const char*)sqlite3_column_text(stmt, 2),
                     sizeof(u->password_hash));
        u->role = sqlite3_column_int(stmt, 3);
        u->enabled = sqlite3_column_int(stmt, 4);
        u->created_at = sqlite3_column_int64(stmt, 5);
        u->last_login = sqlite3_column_int64(stmt, 6);
        u->login_failures = sqlite3_column_int(stmt, 7);
        idx++;
    }

    *count = idx;
    sqlite3_finalize(stmt);
    return RESULT_OK;
}

void auth_user_free_list(auth_user_t *users) {
    free(users);
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

const char* auth_role_to_string(auth_role_t role) {
    switch (role) {
        case AUTH_ROLE_NONE:     return "None";
        case AUTH_ROLE_VIEWER:   return "Viewer";
        case AUTH_ROLE_OPERATOR: return "Operator";
        case AUTH_ROLE_ADMIN:    return "Admin";
        default:                 return "Unknown";
    }
}
