/**
 * @file auth.c
 * @brief Authentication and session management implementation
 *
 * Security Notes:
 * - Password comparison uses constant-time algorithm to prevent timing attacks
 * - Supports both local users (SQLite) and synced users (PROFINET)
 * - Synced users have priority if user_sync module has valid credentials
 */

#include "auth.h"
#include "user_sync.h"
#include "utils/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Global session state */
auth_session_t g_auth_session = {0};

/* Default credentials - Water treatment puns! */
/*
 * SECURITY: Default credentials removed - see auth_init() comment.
 * Default user is synced from Controller, not hardcoded locally.
 */
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

/**
 * Constant-time string comparison to prevent timing attacks
 *
 * Always compares all bytes regardless of where mismatch occurs.
 * This prevents attackers from determining hash values via timing analysis.
 */
static bool constant_time_compare(const char *a, const char *b, size_t len) {
    if (!a || !b) return false;

    volatile uint8_t result = 0;

    for (size_t i = 0; i < len; i++) {
        result |= ((uint8_t)a[i] ^ (uint8_t)b[i]);
    }

    return result == 0;
}

static bool verify_password(const char *password, const char *stored_hash) {
    char computed_hash[AUTH_MAX_HASH];
    auth_hash_password(password, DEFAULT_SALT, computed_hash);

    /* Use constant-time comparison to prevent timing attacks */
    size_t computed_len = strlen(computed_hash);
    size_t stored_len = strlen(stored_hash);

    /* Ensure we compare the full length to avoid early termination */
    size_t max_len = (computed_len > stored_len) ? computed_len : stored_len;
    if (max_len > AUTH_MAX_HASH) max_len = AUTH_MAX_HASH;

    /* Length mismatch is a failure, but still do full comparison */
    bool length_match = (computed_len == stored_len);

    return length_match && constant_time_compare(computed_hash, stored_hash, max_len);
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

/*
 * SECURITY NOTE: Default admin creation has been removed.
 *
 * The default user ("admin") exists in the Controller's database
 * and is synced to RTUs via PROFINET. The RTU no longer creates
 * a hardcoded local admin account.
 *
 * This eliminates the backdoor password vulnerability and ensures
 * the Controller is the single source of truth for credentials.
 */

/* ============================================================================
 * Authentication API Implementation
 * ========================================================================== */

result_t auth_init(database_t *db) {
    CHECK_NULL(db);

    /* Ensure users table exists (for local user management via TUI) */
    if (ensure_users_table(db) != RESULT_OK) {
        return RESULT_ERROR;
    }

    /*
     * SECURITY: Do NOT create default admin locally.
     *
     * The default user ("admin") exists in the Controller's database
     * and is synced to RTUs like any other user. The RTU should:
     *
     * 1. Start with empty user store on fresh install
     * 2. Receive default user via first Controller sync
     * 3. Persist synced users to NV memory
     * 4. If no users available: DENY all authentication (fail-safe)
     *
     * This ensures the Controller remains the single source of truth
     * for credentials and prevents hardcoded backdoor passwords.
     */
    int local_user_count = count_users(db);

    /* Initialize user sync for PROFINET-synced credentials */
    result_t r = user_sync_init();
    if (r != RESULT_OK) {
        LOG_WARNING("User sync initialization failed - synced users will not work");
        /* Non-fatal - continue with local auth only */
    } else {
        /* Try to load persisted users from NV storage */
        result_t nv_result = user_sync_load_from_nv();
        if (nv_result == RESULT_OK) {
            LOG_INFO("User sync: Restored users from NV storage");
        } else if (nv_result == RESULT_NOT_FOUND) {
            LOG_INFO("User sync: No persisted users, awaiting controller sync");
        }
    }

    /* Clear any existing session */
    memset(&g_auth_session, 0, sizeof(g_auth_session));

    /* Check if we have any authentication source available */
    int synced_user_count = user_sync_get_user_count();
    if (synced_user_count == 0 && local_user_count == 0) {
        LOG_WARNING("No users available - awaiting controller sync. "
                    "All authentication will be denied until sync received.");
    }

    LOG_INFO("Authentication system initialized (%d synced, %d local users)",
             synced_user_count, local_user_count);
    return RESULT_OK;
}

/**
 * Convert user_sync_role_t to auth_role_t
 */
static auth_role_t convert_sync_role(user_sync_role_t sync_role) {
    switch (sync_role) {
        case USER_SYNC_ROLE_NONE:     return AUTH_ROLE_NONE;
        case USER_SYNC_ROLE_VIEWER:   return AUTH_ROLE_VIEWER;
        case USER_SYNC_ROLE_OPERATOR: return AUTH_ROLE_OPERATOR;
        case USER_SYNC_ROLE_ADMIN:    return AUTH_ROLE_ADMIN;
        default:                      return AUTH_ROLE_NONE;
    }
}

result_t auth_login(database_t *db, const char *username, const char *password) {
    CHECK_NULL(db);
    CHECK_NULL(username);
    CHECK_NULL(password);

    /*
     * SECURITY: Check if we're awaiting initial controller sync.
     *
     * If no users are available (synced or local), deny all authentication.
     * This is fail-safe behavior - no hardcoded backdoor.
     * TUI should display "Awaiting controller sync" message.
     */
    if (user_sync_awaiting_initial_sync() && count_users(db) == 0) {
        LOG_WARNING("Auth denied for '%s': awaiting controller sync (no users available)",
                    username);
        return RESULT_NOT_FOUND;
    }

    /*
     * Priority 1: Check synced users from PROFINET
     * These are credentials pushed from the central SCADA controller.
     * If user_sync has users, we authenticate against those first.
     */
    if (user_sync_has_users()) {
        user_sync_role_t sync_role;
        if (user_sync_authenticate(username, password, &sync_role)) {
            /* Synced user authenticated successfully */
            g_auth_session.authenticated = true;
            g_auth_session.user.id = -1;  /* Negative ID indicates synced user */
            SAFE_STRNCPY(g_auth_session.user.username, username,
                         sizeof(g_auth_session.user.username));
            g_auth_session.user.role = convert_sync_role(sync_role);
            g_auth_session.user.enabled = true;
            g_auth_session.login_time = time(NULL);
            g_auth_session.last_activity = g_auth_session.login_time;

            LOG_INFO("User '%s' logged in via synced credentials (role: %s)",
                     username, auth_role_to_string(g_auth_session.user.role));
            return RESULT_OK;
        }
        /* Synced user auth failed - continue to local database check */
        LOG_DEBUG("Synced user auth failed for '%s', checking local database", username);
    }

    /*
     * Priority 2: Check local database users
     * These are users created directly on the RTU via TUI.
     */
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

bool auth_awaiting_controller_sync(database_t *db) {
    /* Check if user sync is awaiting initial sync */
    if (!user_sync_awaiting_initial_sync()) {
        return false;  /* Have synced users */
    }

    /* Check if we have any local users */
    if (db && count_users(db) > 0) {
        return false;  /* Have local users */
    }

    return true;  /* No users at all - awaiting sync */
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
