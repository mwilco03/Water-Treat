/**
 * @file auth.h
 * @brief Authentication and session management
 *
 * Simple authentication for the Water Treatment RTU.
 * Default credentials: admin / H2OhYeah!
 */

#ifndef AUTH_H
#define AUTH_H

#include "common.h"
#include "db/database.h"
#include <stdbool.h>
#include <time.h>

/* Maximum lengths */
#define AUTH_MAX_USERNAME       32
#define AUTH_MAX_PASSWORD       64
#define AUTH_MAX_HASH           128
#define AUTH_SESSION_TIMEOUT    3600    /* 1 hour */

/* User roles */
typedef enum {
    AUTH_ROLE_NONE = 0,
    AUTH_ROLE_VIEWER,       /* Read-only access */
    AUTH_ROLE_OPERATOR,     /* Can control actuators, ack alarms */
    AUTH_ROLE_ADMIN,        /* Full access including config */
} auth_role_t;

/* User record */
typedef struct {
    int id;
    char username[AUTH_MAX_USERNAME];
    char password_hash[AUTH_MAX_HASH];
    auth_role_t role;
    bool enabled;
    time_t created_at;
    time_t last_login;
    int login_failures;
} auth_user_t;

/* Active session */
typedef struct {
    bool authenticated;
    auth_user_t user;
    time_t login_time;
    time_t last_activity;
} auth_session_t;

/* Global session (single-user TUI) */
extern auth_session_t g_auth_session;

/* ============================================================================
 * Authentication API
 * ========================================================================== */

/**
 * Initialize authentication system
 * Creates default admin user if no users exist
 */
result_t auth_init(database_t *db);

/**
 * Attempt login with username and password
 * @return RESULT_OK on success, RESULT_ERROR on failure
 */
result_t auth_login(database_t *db, const char *username, const char *password);

/**
 * Logout current session
 */
void auth_logout(void);

/**
 * Check if currently authenticated
 */
bool auth_is_logged_in(void);

/**
 * Check if current user has required role
 */
bool auth_has_role(auth_role_t required_role);

/**
 * Get current username (or "anonymous" if not logged in)
 */
const char* auth_get_username(void);

/**
 * Update last activity timestamp (call on user input)
 */
void auth_touch_session(void);

/**
 * Check for session timeout
 * @return true if session expired (auto-logout performed)
 */
bool auth_check_timeout(void);

/* ============================================================================
 * User Management API
 * ========================================================================== */

/**
 * Create a new user
 */
result_t auth_user_create(database_t *db, const char *username,
                          const char *password, auth_role_t role);

/**
 * Change user password
 */
result_t auth_user_change_password(database_t *db, int user_id,
                                   const char *new_password);

/**
 * Delete user
 */
result_t auth_user_delete(database_t *db, int user_id);

/**
 * List all users
 */
result_t auth_user_list(database_t *db, auth_user_t **users, int *count);

/**
 * Free user list
 */
void auth_user_free_list(auth_user_t *users);

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

/**
 * Get role name string
 */
const char* auth_role_to_string(auth_role_t role);

/**
 * Hash a password (simple hash for embedded use)
 */
void auth_hash_password(const char *password, const char *salt, char *hash_out);

#endif /* AUTH_H */
