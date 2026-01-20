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
 * Constants
 * ============================================================================ */

/** Maximum users that can be stored in RTU (embedded constraint) */
#define USER_SYNC_MAX_USERS         16

/** Maximum username length (matches auth.h) */
#define USER_SYNC_MAX_USERNAME      32

/** Maximum hash string length: "DJB2:XXXXXXXX:XXXXXXXX" + null */
#define USER_SYNC_MAX_HASH          24

/** Salt used for password hashing (must match controller) */
#define USER_SYNC_SALT              "NaCl4Life"

/** PROFINET record index for user sync data (vendor-specific range) */
#define USER_SYNC_PROFINET_INDEX    0xF840

/** Magic header for user sync packets */
#define USER_SYNC_MAGIC             0x55534552  /* "USER" in ASCII */

/** Protocol version for user sync format */
#define USER_SYNC_VERSION           1

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * User roles for RTU local access control
 * Must match controller auth_role_t values
 */
typedef enum {
    USER_SYNC_ROLE_NONE     = 0,    /**< No access */
    USER_SYNC_ROLE_VIEWER   = 1,    /**< Read-only access */
    USER_SYNC_ROLE_OPERATOR = 2,    /**< Can control actuators, ack alarms */
    USER_SYNC_ROLE_ADMIN    = 3,    /**< Full access including config */
} user_sync_role_t;

/**
 * Synced user credential entry
 * Stored in static array, no heap allocation
 */
typedef struct {
    uint32_t user_id;                           /**< Unique ID from controller */
    char username[USER_SYNC_MAX_USERNAME];      /**< Username for login */
    char password_hash[USER_SYNC_MAX_HASH];     /**< DJB2:%08X:%08X format */
    user_sync_role_t role;                      /**< Access level */
    bool active;                                /**< Account enabled flag */
    bool sync_to_rtus;                          /**< Controller marked for RTU sync */
    uint32_t sync_timestamp;                    /**< When last synced (epoch) */
    bool valid;                                 /**< Slot in use flag */
} user_sync_entry_t;

/**
 * User sync packet header (from controller via PROFINET)
 * All multi-byte fields are big-endian (network byte order)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /**< USER_SYNC_MAGIC */
    uint8_t  version;       /**< Protocol version */
    uint8_t  operation;     /**< 0=full sync, 1=add/update, 2=delete */
    uint16_t user_count;    /**< Number of users in packet */
    uint32_t timestamp;     /**< Sync timestamp (epoch seconds) */
    uint16_t checksum;      /**< CRC16-CCITT of payload */
    uint16_t reserved;      /**< Reserved for alignment */
} user_sync_header_t;

/**
 * User sync packet entry (from controller)
 * Follows header, repeated user_count times
 */
typedef struct __attribute__((packed)) {
    uint32_t user_id;                           /**< User ID from controller DB */
    char     username[USER_SYNC_MAX_USERNAME];  /**< Null-terminated username */
    char     password_hash[USER_SYNC_MAX_HASH]; /**< DJB2:%08X:%08X */
    uint8_t  role;                              /**< user_sync_role_t value */
    uint8_t  active;                            /**< 1=enabled, 0=disabled */
    uint8_t  sync_to_rtus;                      /**< 1=sync to RTU, 0=skip */
    uint8_t  reserved;                          /**< Padding for alignment */
} user_sync_packet_entry_t;

/**
 * User sync operation types
 */
typedef enum {
    USER_SYNC_OP_FULL_SYNC  = 0,    /**< Replace all users */
    USER_SYNC_OP_ADD_UPDATE = 1,    /**< Add or update specific users */
    USER_SYNC_OP_DELETE     = 2,    /**< Delete specific users */
} user_sync_operation_t;

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
 * is received (index USER_SYNC_PROFINET_INDEX).
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
 * Hash Utility Functions
 * ============================================================================ */

/**
 * Compute DJB2 hash of a string
 *
 * Standard DJB2 algorithm with 32-bit masking:
 *   hash = 5381
 *   for each char c: hash = ((hash << 5) + hash) + c
 *   return hash & 0xFFFFFFFF
 *
 * @param str   String to hash
 * @return 32-bit hash value
 */
uint32_t user_sync_djb2_hash(const char *str);

/**
 * Generate password hash in controller-compatible format
 *
 * Format: "DJB2:%08X:%08X" where:
 *   - First hash is djb2(salt)
 *   - Second hash is djb2(salt + password)
 *
 * @param password      Plaintext password
 * @param[out] hash_out Buffer for hash string (min USER_SYNC_MAX_HASH bytes)
 */
void user_sync_hash_password(const char *password, char *hash_out);

/**
 * Constant-time string comparison
 *
 * Compares two strings in constant time to prevent timing attacks.
 * Always compares all bytes even if mismatch found early.
 *
 * @param a     First string
 * @param b     Second string
 * @param len   Maximum length to compare
 * @return true if strings match (up to len or first null), false otherwise
 */
bool user_sync_constant_time_compare(const char *a, const char *b, size_t len);

/**
 * Convert role to string for logging/display
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
 * Hash Verification / Test Functions
 * ============================================================================ */

/**
 * Compute DJB2 hashes for password with salt (for verification)
 *
 * Use this to verify RTU hash computation matches controller.
 *
 * @param password      Password to hash
 * @param[out] salt_hash    DJB2 hash of salt alone
 * @param[out] pass_hash    DJB2 hash of salt+password
 */
void user_sync_hash_with_salt(const char *password,
                               uint32_t *salt_hash,
                               uint32_t *pass_hash);

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
