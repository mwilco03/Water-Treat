/**
 * @file user_sync.h
 * @brief RTU-side user credential synchronization via PROFINET
 *
 * Handles reception and validation of user credentials synced from
 * the SCADA controller via PROFINET acyclic data (record writes).
 *
 * Security Design:
 * - Constant-time hash comparison to prevent timing attacks
 * - Static allocation (16 user max) - no heap after init
 * - Fail-safe defaults (deny on any error)
 * - DJB2 hash format compatible with controller: "DJB2:%08X:%08X"
 *
 * Protocol Definition:
 *   Canonical definitions are in user_sync_protocol.h (from Water-Controller)
 *   See: docs/RTU_SHARED_PROTOCOL_SYNC.md
 *
 * Controller Format Reference (web/api/app/persistence/users.py):
 *   Hash: _djb2_hash() with 32-bit overflow masking
 *   Salt: USER_SYNC_SALT = "NaCl4Life"
 *   Format: "DJB2:<salt_hash>:<password_hash>"
 *   Endpoint: GET /api/v1/users/sync
 */

#ifndef USER_SYNC_H
#define USER_SYNC_H

#include "common.h"
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Shared Protocol Definitions (from Water-Controller)
 * ============================================================================
 * These types, constants, and inline functions are defined in the shared
 * protocol header fetched from the Water-Controller repository at build time.
 * This ensures RTU and Controller use identical wire format definitions.
 *
 * Provided by user_sync_protocol.h:
 *   Constants:
 *     USER_SYNC_PROTOCOL_VERSION, USER_SYNC_MAGIC, USER_SYNC_RECORD_INDEX
 *     USER_SYNC_MAX_USERS, USER_SYNC_USERNAME_LEN, USER_SYNC_HASH_LEN
 *     USER_SYNC_SALT, DJB2_INIT
 *     USER_SYNC_OP_FULL_SYNC, USER_SYNC_OP_ADD_UPDATE, USER_SYNC_OP_DELETE
 *     USER_ROLE_VIEWER, USER_ROLE_OPERATOR, USER_ROLE_ENGINEER, USER_ROLE_ADMIN
 *     USER_FLAG_ACTIVE, USER_FLAG_SYNC_TO_RTUS
 *
 *   Types:
 *     user_sync_role_t, user_sync_header_t, user_sync_record_t
 *     user_sync_payload_t, user_sync_result_t
 *
 *   Inline functions:
 *     user_sync_djb2(), user_sync_hash_with_salt(), user_sync_format_hash()
 *     user_sync_constant_time_compare(), user_sync_crc16_ccitt()
 *     user_sync_validate_header(), user_sync_validate_payload()
 *     user_sync_init_header(), user_sync_result_str(), user_sync_role_str()
 *     user_sync_op_str(), user_sync_role_sufficient(), user_sync_payload_size()
 *
 * See: docs/RTU_SHARED_PROTOCOL_SYNC.md
 */
#include "user_sync_protocol.h"

/* ============================================================================
 * RTU-Local Aliases for Backward Compatibility
 * ============================================================================
 * Map existing RTU code names to shared protocol names.
 * New code should use the shared protocol names directly.
 */

/** Maximum username length - alias for shared protocol constant */
#define USER_SYNC_MAX_USERNAME      USER_SYNC_USERNAME_LEN

/** Maximum hash string length - alias for shared protocol constant */
#define USER_SYNC_MAX_HASH          USER_SYNC_HASH_LEN

/** PROFINET record index - alias for shared protocol constant */
#define USER_SYNC_PROFINET_INDEX    USER_SYNC_RECORD_INDEX

/** Protocol version - alias for shared protocol constant */
#define USER_SYNC_VERSION           USER_SYNC_PROTOCOL_VERSION

/* ============================================================================
 * RTU-Local Types
 * ============================================================================
 * These types are specific to the RTU implementation and not shared.
 */

/**
 * Synced user credential entry (RTU local storage format)
 *
 * This is the RTU's internal representation of a user.
 * Differs from user_sync_record_t (wire format) in having additional
 * local state like sync_timestamp and valid flag.
 */
typedef struct {
    uint32_t user_id;                           /**< Unique ID from controller */
    char username[USER_SYNC_USERNAME_LEN];      /**< Username for login */
    char password_hash[USER_SYNC_HASH_LEN];     /**< DJB2:%08X:%08X format */
    user_sync_role_t role;                      /**< Access level */
    bool active;                                /**< Account enabled flag */
    bool sync_to_rtus;                          /**< Controller marked for RTU sync */
    uint32_t sync_timestamp;                    /**< When last synced (epoch) */
    bool valid;                                 /**< Slot in use flag */
} user_sync_entry_t;

/**
 * Sync status for diagnostics
 */
typedef struct {
    uint32_t last_sync_time;        /**< Timestamp of last successful sync */
    uint32_t sync_count;            /**< Total successful syncs */
    uint32_t error_count;           /**< Total sync errors */
    uint32_t users_stored;          /**< Current number of valid users */
    uint32_t last_error_code;       /**< Last error result code */
} user_sync_status_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Initialize user sync subsystem
 *
 * Allocates static storage for up to USER_SYNC_MAX_USERS.
 * Must be called once during system initialization.
 *
 * @return RESULT_OK on success
 */
result_t user_sync_init(void);

/**
 * Shutdown user sync subsystem
 *
 * Clears all stored credentials from memory.
 */
void user_sync_shutdown(void);

/**
 * Process incoming user sync packet from PROFINET
 *
 * Called from profinet_write_callback() when a user sync record
 * is received (index USER_SYNC_RECORD_INDEX).
 *
 * @param data      Raw packet data
 * @param length    Packet length in bytes
 * @return RESULT_OK on successful processing
 *         RESULT_INVALID_PARAM if packet is malformed
 *         RESULT_ERROR on processing failure
 */
result_t user_sync_process_packet(const uint8_t *data, uint16_t length);

/**
 * Authenticate a user against synced credentials
 *
 * Uses constant-time comparison to prevent timing attacks.
 * Implements fail-safe: any error returns false.
 *
 * @param username      Username to authenticate
 * @param password      Plaintext password to verify
 * @param[out] role     If non-NULL, receives user's role on success
 * @return true if authentication successful, false otherwise
 */
bool user_sync_authenticate(const char *username, const char *password,
                            user_sync_role_t *role);

/**
 * Look up a user by username
 *
 * @param username      Username to find
 * @return Pointer to user entry, or NULL if not found
 *         Note: Returns const pointer - do not modify
 */
const user_sync_entry_t* user_sync_find_user(const char *username);

/**
 * Get current sync status for diagnostics
 *
 * @param[out] status   Status structure to fill
 * @return RESULT_OK on success
 */
result_t user_sync_get_status(user_sync_status_t *status);

/**
 * Get number of currently stored users
 *
 * @return Number of valid user entries (0 to USER_SYNC_MAX_USERS)
 */
int user_sync_get_user_count(void);

/**
 * Get user entry by index (for enumeration)
 *
 * @param index     Index (0 to USER_SYNC_MAX_USERS-1)
 * @return Pointer to user entry, or NULL if index invalid or slot empty
 */
const user_sync_entry_t* user_sync_get_user(int index);

/**
 * Clear all synced users
 *
 * Used for security reset or testing.
 */
void user_sync_clear_all(void);

/**
 * Check if user sync has valid credentials
 *
 * @return true if at least one user is synced and valid
 */
bool user_sync_has_users(void);

/* ============================================================================
 * Hash Utility Functions (RTU-specific wrappers)
 * ============================================================================
 * These wrap the shared inline functions for compatibility with existing code.
 */

/**
 * Compute DJB2 hash of a string
 *
 * Wrapper around user_sync_djb2() from shared header.
 *
 * @param str   String to hash
 * @return 32-bit hash value
 */
uint32_t user_sync_djb2_hash(const char *str);

/**
 * Generate password hash in controller-compatible format
 *
 * Wrapper around user_sync_format_hash() from shared header.
 *
 * @param password      Plaintext password
 * @param[out] hash_out Buffer for hash string (min USER_SYNC_HASH_LEN bytes)
 */
void user_sync_hash_password(const char *password, char *hash_out);

/**
 * Convert role to string for logging/display
 *
 * Wrapper around user_sync_role_str() from shared header.
 *
 * @param role  Role value
 * @return Static string representation
 */
const char* user_sync_role_to_string(user_sync_role_t role);

/* ============================================================================
 * Non-Volatile Storage Backend Interface
 * ============================================================================ */

/**
 * NV storage operations for persistent user credential storage
 *
 * Implement these for your hardware (EEPROM, Flash, FRAM, etc.)
 * If not set, users are stored in RAM only (lost on reboot).
 */
typedef struct {
    /**
     * Read data from NV storage
     * @param offset  Byte offset from user storage base
     * @param data    Buffer to read into
     * @param len     Number of bytes to read
     * @return 0 on success, -1 on error
     */
    int (*read)(uint32_t offset, void *data, size_t len);

    /**
     * Write data to NV storage
     * @param offset  Byte offset from user storage base
     * @param data    Data to write
     * @param len     Number of bytes to write
     * @return 0 on success, -1 on error
     */
    int (*write)(uint32_t offset, const void *data, size_t len);

    /**
     * Flush/sync writes to physical storage (optional)
     * @return 0 on success, -1 on error
     */
    int (*flush)(void);
} user_sync_nv_ops_t;

/**
 * Register NV storage backend
 *
 * Call during init to enable persistent storage.
 * If not called, users are RAM-only.
 *
 * @param ops  NV operations structure (must remain valid)
 * @return RESULT_OK on success
 */
result_t user_sync_set_nv_backend(const user_sync_nv_ops_t *ops);

/**
 * Load users from NV storage
 *
 * Call after init and setting NV backend to restore persisted users.
 *
 * @return RESULT_OK on success, RESULT_NOT_FOUND if no stored users
 */
result_t user_sync_load_from_nv(void);

/**
 * Save users to NV storage
 *
 * Called automatically after processing sync packets if NV backend is set.
 *
 * @return RESULT_OK on success
 */
result_t user_sync_save_to_nv(void);

/* ============================================================================
 * Test/Verification Functions
 * ============================================================================ */

/**
 * Verify hash implementation against known test vectors
 *
 * @return true if implementation matches expected values
 */
bool user_sync_verify_hash_implementation(void);

/**
 * Check if user sync is awaiting initial controller sync
 *
 * Returns true if:
 * - No users are currently stored
 * - No sync has been received from controller
 *
 * TUI should display "Awaiting controller sync" in this state.
 *
 * @return true if awaiting sync, false if users available
 */
bool user_sync_awaiting_initial_sync(void);

#endif /* USER_SYNC_H */
