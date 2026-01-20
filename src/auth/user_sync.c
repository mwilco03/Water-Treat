/**
 * @file user_sync.c
 * @brief RTU-side user credential synchronization implementation
 *
 * Handles PROFINET-based user credential sync from SCADA controller.
 *
 * Security Implementation Notes:
 * - All comparisons use constant-time functions to prevent timing attacks
 * - Static allocation only - no heap usage after initialization
 * - Fail-safe: any error condition results in access denial
 * - Hash format matches controller: "DJB2:%08X:%08X"
 */

#include "user_sync.h"
#include "utils/logger.h"
#include <string.h>
#include <arpa/inet.h>  /* ntohl, ntohs for network byte order */
#include <time.h>

/* ============================================================================
 * Static Storage (no heap allocation)
 * ============================================================================ */

/** Static array for user credentials */
static user_sync_entry_t g_users[USER_SYNC_MAX_USERS];

/** Sync status tracking */
static user_sync_status_t g_status = {0};

/** Initialization flag */
static bool g_initialized = false;

/** Mutex would go here for thread safety - using simple flag for now */
static volatile bool g_sync_in_progress = false;

/** NV storage backend (NULL = RAM only) */
static const user_sync_nv_ops_t *g_nv_ops = NULL;

/** Flag to track if we've ever received a sync from controller */
static bool g_received_initial_sync = false;

/** NV storage magic for validation */
#define USER_SYNC_NV_MAGIC      0x55534E56  /* "USNV" */
#define USER_SYNC_NV_VERSION    1

/** NV storage header */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  user_count;
    uint16_t checksum;
} user_sync_nv_header_t;

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * Find a free slot in the user array
 * @return Index of free slot, or -1 if full
 */
static int find_free_slot(void) {
    for (int i = 0; i < USER_SYNC_MAX_USERS; i++) {
        if (!g_users[i].valid) {
            return i;
        }
    }
    return -1;
}

/**
 * Find user by ID
 * @return Index of user, or -1 if not found
 */
static int find_user_by_id(uint32_t user_id) {
    for (int i = 0; i < USER_SYNC_MAX_USERS; i++) {
        if (g_users[i].valid && g_users[i].user_id == user_id) {
            return i;
        }
    }
    return -1;
}

/**
 * Find user by username
 * Uses constant-time comparison
 * @return Index of user, or -1 if not found
 */
static int find_user_by_name(const char *username) {
    if (!username) return -1;

    for (int i = 0; i < USER_SYNC_MAX_USERS; i++) {
        if (g_users[i].valid) {
            if (user_sync_constant_time_compare(g_users[i].username, username,
                                                 USER_SYNC_MAX_USERNAME)) {
                return i;
            }
        }
    }
    return -1;
}

/**
 * CRC16-CCITT for packet validation
 * Polynomial: 0x1021, Initial: 0xFFFF
 * This matches the controller's implementation
 */
static uint16_t compute_crc16_ccitt(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < length; i++) {
        crc ^= ((uint16_t)data[i] << 8);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

/**
 * Validate packet header
 */
static bool validate_header(const user_sync_header_t *hdr, uint16_t total_len) {
    /* Check magic */
    uint32_t magic = ntohl(hdr->magic);
    if (magic != USER_SYNC_MAGIC) {
        LOG_WARNING("User sync: invalid magic 0x%08X (expected 0x%08X)",
                    magic, USER_SYNC_MAGIC);
        return false;
    }

    /* Check version */
    if (hdr->version != USER_SYNC_VERSION) {
        LOG_WARNING("User sync: unsupported version %d (expected %d)",
                    hdr->version, USER_SYNC_VERSION);
        return false;
    }

    /* Check packet size makes sense */
    uint16_t user_count = ntohs(hdr->user_count);
    size_t expected_size = sizeof(user_sync_header_t) +
                           (user_count * sizeof(user_sync_packet_entry_t));

    if (total_len < expected_size) {
        LOG_WARNING("User sync: packet too short (%u < %zu)",
                    total_len, expected_size);
        return false;
    }

    /* Validate user count */
    if (user_count > USER_SYNC_MAX_USERS) {
        LOG_WARNING("User sync: too many users %u (max %d)",
                    user_count, USER_SYNC_MAX_USERS);
        return false;
    }

    return true;
}

/**
 * Process a single user entry from sync packet
 */
static result_t process_user_entry(const user_sync_packet_entry_t *entry,
                                   user_sync_operation_t operation) {
    uint32_t user_id = ntohl(entry->user_id);

    /* Skip users not marked for RTU sync */
    if (!entry->sync_to_rtus) {
        LOG_DEBUG("User sync: skipping user ID %u (not marked for RTU sync)", user_id);
        return RESULT_OK;
    }

    /* Validate username (must be null-terminated and non-empty) */
    if (entry->username[0] == '\0') {
        LOG_WARNING("User sync: empty username for ID %u", user_id);
        return RESULT_INVALID_PARAM;
    }

    /* Ensure null termination (paranoid check) */
    char username[USER_SYNC_MAX_USERNAME];
    memcpy(username, entry->username, USER_SYNC_MAX_USERNAME - 1);
    username[USER_SYNC_MAX_USERNAME - 1] = '\0';

    /* Handle delete operation */
    if (operation == USER_SYNC_OP_DELETE) {
        int idx = find_user_by_id(user_id);
        if (idx >= 0) {
            /* Securely clear the entry */
            memset(&g_users[idx], 0, sizeof(user_sync_entry_t));
            g_users[idx].valid = false;
            LOG_INFO("User sync: deleted user '%s' (ID %u)", username, user_id);
        }
        return RESULT_OK;
    }

    /* Add or update operation */
    int idx = find_user_by_id(user_id);
    if (idx < 0) {
        /* New user - find free slot */
        idx = find_free_slot();
        if (idx < 0) {
            LOG_WARNING("User sync: storage full, cannot add user '%s'", username);
            return RESULT_NO_MEMORY;
        }
    }

    /* Populate user entry */
    user_sync_entry_t *user = &g_users[idx];
    user->user_id = user_id;
    SAFE_STRNCPY(user->username, username, sizeof(user->username));

    /* Copy and validate hash format */
    char hash[USER_SYNC_MAX_HASH];
    memcpy(hash, entry->password_hash, USER_SYNC_MAX_HASH - 1);
    hash[USER_SYNC_MAX_HASH - 1] = '\0';

    /* Validate hash format: "DJB2:XXXXXXXX:XXXXXXXX" */
    if (strncmp(hash, "DJB2:", 5) != 0 || strlen(hash) < 22) {
        LOG_WARNING("User sync: invalid hash format for user '%s'", username);
        return RESULT_INVALID_PARAM;
    }
    SAFE_STRNCPY(user->password_hash, hash, sizeof(user->password_hash));

    /* Set role with validation */
    if (entry->role > USER_SYNC_ROLE_ADMIN) {
        LOG_WARNING("User sync: invalid role %d for user '%s', defaulting to VIEWER",
                    entry->role, username);
        user->role = USER_SYNC_ROLE_VIEWER;
    } else {
        user->role = (user_sync_role_t)entry->role;
    }

    user->active = (entry->active != 0);
    user->sync_to_rtus = true;  /* Already validated above */
    user->sync_timestamp = (uint32_t)time(NULL);
    user->valid = true;

    LOG_INFO("User sync: %s user '%s' (ID %u, role=%s, active=%d)",
             (find_user_by_id(user_id) == idx) ? "updated" : "added",
             username, user_id, user_sync_role_to_string(user->role), user->active);

    return RESULT_OK;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

result_t user_sync_init(void) {
    if (g_initialized) {
        return RESULT_OK;
    }

    /* Clear all storage */
    memset(g_users, 0, sizeof(g_users));
    memset(&g_status, 0, sizeof(g_status));

    g_initialized = true;
    LOG_INFO("User sync initialized (max %d users)", USER_SYNC_MAX_USERS);

    return RESULT_OK;
}

void user_sync_shutdown(void) {
    if (!g_initialized) return;

    /* Securely clear all credentials */
    for (int i = 0; i < USER_SYNC_MAX_USERS; i++) {
        if (g_users[i].valid) {
            /* Explicit memory clearing for security */
            memset(g_users[i].password_hash, 0, sizeof(g_users[i].password_hash));
            memset(g_users[i].username, 0, sizeof(g_users[i].username));
        }
    }
    memset(g_users, 0, sizeof(g_users));

    g_initialized = false;
    LOG_INFO("User sync shutdown complete");
}

result_t user_sync_process_packet(const uint8_t *data, uint16_t length) {
    if (!g_initialized) {
        return RESULT_NOT_INITIALIZED;
    }

    if (!data || length < sizeof(user_sync_header_t)) {
        g_status.error_count++;
        g_status.last_error_code = RESULT_INVALID_PARAM;
        return RESULT_INVALID_PARAM;
    }

    /* Prevent concurrent processing */
    if (g_sync_in_progress) {
        LOG_WARNING("User sync: already in progress, dropping packet");
        return RESULT_BUSY;
    }
    g_sync_in_progress = true;

    /* Parse header */
    const user_sync_header_t *hdr = (const user_sync_header_t *)data;

    /* Validate header */
    if (!validate_header(hdr, length)) {
        g_status.error_count++;
        g_status.last_error_code = RESULT_INVALID_PARAM;
        g_sync_in_progress = false;
        return RESULT_INVALID_PARAM;
    }

    /* Verify checksum (CRC16-CCITT) */
    uint16_t stored_checksum = ntohs(hdr->checksum);
    size_t payload_offset = sizeof(user_sync_header_t);
    size_t payload_len = length - payload_offset;
    uint16_t computed_checksum = compute_crc16_ccitt(data + payload_offset, payload_len);

    if (stored_checksum != computed_checksum) {
        LOG_WARNING("User sync: checksum mismatch (got 0x%04X, expected 0x%04X)",
                    computed_checksum, stored_checksum);
        g_status.error_count++;
        g_status.last_error_code = RESULT_ERROR;
        g_sync_in_progress = false;
        return RESULT_ERROR;
    }

    user_sync_operation_t operation = (user_sync_operation_t)hdr->operation;
    uint16_t user_count = ntohs(hdr->user_count);

    LOG_INFO("User sync: processing %s with %u users",
             operation == USER_SYNC_OP_FULL_SYNC ? "full sync" :
             operation == USER_SYNC_OP_ADD_UPDATE ? "add/update" : "delete",
             user_count);

    /* For full sync, clear existing users first */
    if (operation == USER_SYNC_OP_FULL_SYNC) {
        user_sync_clear_all();
    }

    /* Process each user entry */
    const user_sync_packet_entry_t *entries =
        (const user_sync_packet_entry_t *)(data + sizeof(user_sync_header_t));

    result_t result = RESULT_OK;
    int processed = 0;
    int errors = 0;

    for (uint16_t i = 0; i < user_count; i++) {
        result_t r = process_user_entry(&entries[i], operation);
        if (r == RESULT_OK) {
            processed++;
        } else {
            errors++;
            if (result == RESULT_OK) {
                result = r;  /* Keep first error */
            }
        }
    }

    /* Update status */
    g_status.last_sync_time = ntohl(hdr->timestamp);
    g_status.sync_count++;
    g_status.users_stored = (uint32_t)user_sync_get_user_count();

    if (errors > 0) {
        g_status.error_count += (uint32_t)errors;
        g_status.last_error_code = result;
    }

    LOG_INFO("User sync complete: %d processed, %d errors, %d total users",
             processed, errors, g_status.users_stored);

    /* Mark that we've received a sync from controller */
    g_received_initial_sync = true;

    /* Persist to NV storage if backend is configured */
    if (g_nv_ops && processed > 0) {
        result_t nv_result = user_sync_save_to_nv();
        if (nv_result != RESULT_OK) {
            LOG_WARNING("User sync: Failed to persist to NV storage");
        }
    }

    g_sync_in_progress = false;
    return result;
}

bool user_sync_authenticate(const char *username, const char *password,
                            user_sync_role_t *role) {
    /* Fail-safe: deny on any error */
    if (!g_initialized || !username || !password) {
        return false;
    }

    /* Find user (constant-time lookup) */
    int idx = find_user_by_name(username);
    if (idx < 0) {
        /* User not found - but still compute hash to prevent timing leak */
        char dummy_hash[USER_SYNC_MAX_HASH];
        user_sync_hash_password(password, dummy_hash);
        return false;
    }

    user_sync_entry_t *user = &g_users[idx];

    /* Check if account is active */
    if (!user->active) {
        LOG_WARNING("User sync auth: user '%s' is disabled", username);
        return false;
    }

    /* Compute hash for provided password */
    char computed_hash[USER_SYNC_MAX_HASH];
    user_sync_hash_password(password, computed_hash);

    /* Constant-time comparison of hashes */
    bool match = user_sync_constant_time_compare(computed_hash,
                                                  user->password_hash,
                                                  USER_SYNC_MAX_HASH);

    if (match) {
        if (role) {
            *role = user->role;
        }
        LOG_INFO("User sync auth: user '%s' authenticated (role=%s)",
                 username, user_sync_role_to_string(user->role));
        return true;
    }

    LOG_WARNING("User sync auth: invalid password for user '%s'", username);
    return false;
}

const user_sync_entry_t* user_sync_find_user(const char *username) {
    if (!g_initialized || !username) {
        return NULL;
    }

    int idx = find_user_by_name(username);
    if (idx < 0) {
        return NULL;
    }

    return &g_users[idx];
}

result_t user_sync_get_status(user_sync_status_t *status) {
    if (!status) {
        return RESULT_INVALID_PARAM;
    }

    *status = g_status;
    status->users_stored = (uint32_t)user_sync_get_user_count();

    return RESULT_OK;
}

int user_sync_get_user_count(void) {
    if (!g_initialized) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < USER_SYNC_MAX_USERS; i++) {
        if (g_users[i].valid) {
            count++;
        }
    }
    return count;
}

const user_sync_entry_t* user_sync_get_user(int index) {
    if (!g_initialized || index < 0 || index >= USER_SYNC_MAX_USERS) {
        return NULL;
    }

    if (!g_users[index].valid) {
        return NULL;
    }

    return &g_users[index];
}

void user_sync_clear_all(void) {
    if (!g_initialized) return;

    /* Securely clear all credentials */
    for (int i = 0; i < USER_SYNC_MAX_USERS; i++) {
        if (g_users[i].valid) {
            memset(g_users[i].password_hash, 0, sizeof(g_users[i].password_hash));
            memset(g_users[i].username, 0, sizeof(g_users[i].username));
        }
        memset(&g_users[i], 0, sizeof(user_sync_entry_t));
    }

    LOG_INFO("User sync: cleared all users");
}

bool user_sync_has_users(void) {
    return user_sync_get_user_count() > 0;
}

/* ============================================================================
 * Hash Utility Functions
 * ============================================================================ */

uint32_t user_sync_djb2_hash(const char *str) {
    if (!str) {
        return 5381;
    }

    uint32_t hash = 5381;
    int c;

    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + (uint32_t)c;  /* hash * 33 + c */
    }

    return hash;
}

void user_sync_hash_password(const char *password, char *hash_out) {
    if (!hash_out) return;

    if (!password) {
        hash_out[0] = '\0';
        return;
    }

    /* Compute salt hash */
    uint32_t salt_hash = user_sync_djb2_hash(USER_SYNC_SALT);

    /* Compute hash of salt + password */
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", USER_SYNC_SALT, password);
    uint32_t password_hash = user_sync_djb2_hash(combined);

    /* Format: "DJB2:%08X:%08X" */
    snprintf(hash_out, USER_SYNC_MAX_HASH, "DJB2:%08X:%08X",
             salt_hash, password_hash);
}

bool user_sync_constant_time_compare(const char *a, const char *b, size_t len) {
    if (!a || !b) {
        return false;
    }

    volatile uint8_t result = 0;
    size_t i;

    /*
     * Always iterate full length to prevent timing attacks.
     * XOR accumulates differences - if any byte differs, result != 0
     */
    for (i = 0; i < len; i++) {
        uint8_t ca = (uint8_t)a[i];
        uint8_t cb = (uint8_t)b[i];
        result |= (ca ^ cb);

        /*
         * Handle null termination consistently.
         * Once we hit null in either string, use null for comparison.
         * This ensures we still iterate the full length.
         */
        if (a[i] == '\0' || b[i] == '\0') {
            /* Check if both terminated at same point */
            if (a[i] != b[i]) {
                result |= 0xFF;  /* Strings have different lengths */
            }
            /* Continue iterating with nulls to maintain constant time */
            for (i++; i < len; i++) {
                result |= 0;  /* No-op but compiler can't optimize away */
            }
            break;
        }
    }

    return result == 0;
}

const char* user_sync_role_to_string(user_sync_role_t role) {
    switch (role) {
        case USER_SYNC_ROLE_NONE:     return "None";
        case USER_SYNC_ROLE_VIEWER:   return "Viewer";
        case USER_SYNC_ROLE_OPERATOR: return "Operator";
        case USER_SYNC_ROLE_ADMIN:    return "Admin";
        default:                      return "Unknown";
    }
}

/* ============================================================================
 * NV Storage Backend Functions
 * ============================================================================ */

result_t user_sync_set_nv_backend(const user_sync_nv_ops_t *ops) {
    if (!ops) {
        g_nv_ops = NULL;
        LOG_INFO("User sync: NV backend disabled (RAM-only mode)");
        return RESULT_OK;
    }

    if (!ops->read || !ops->write) {
        LOG_ERROR("User sync: NV backend missing required read/write ops");
        return RESULT_INVALID_PARAM;
    }

    g_nv_ops = ops;
    LOG_INFO("User sync: NV backend registered");
    return RESULT_OK;
}

/**
 * Compute checksum for NV data validation
 */
static uint16_t compute_nv_checksum(const user_sync_entry_t *users, int count) {
    uint16_t checksum = 0;
    const uint8_t *data = (const uint8_t *)users;
    size_t len = (size_t)count * sizeof(user_sync_entry_t);

    for (size_t i = 0; i < len; i++) {
        checksum += data[i];
    }
    return checksum;
}

result_t user_sync_load_from_nv(void) {
    if (!g_initialized) {
        return RESULT_NOT_INITIALIZED;
    }

    if (!g_nv_ops || !g_nv_ops->read) {
        LOG_DEBUG("User sync: No NV backend, skipping load");
        return RESULT_NOT_FOUND;
    }

    /* Read header */
    user_sync_nv_header_t header;
    if (g_nv_ops->read(0, &header, sizeof(header)) != 0) {
        LOG_WARNING("User sync: Failed to read NV header");
        return RESULT_IO_ERROR;
    }

    /* Validate magic */
    if (header.magic != USER_SYNC_NV_MAGIC) {
        LOG_INFO("User sync: NV storage empty or corrupted (no valid magic)");
        return RESULT_NOT_FOUND;
    }

    /* Validate version */
    if (header.version != USER_SYNC_NV_VERSION) {
        LOG_WARNING("User sync: NV version mismatch (%d != %d), ignoring stored data",
                    header.version, USER_SYNC_NV_VERSION);
        return RESULT_NOT_FOUND;
    }

    /* Validate count */
    if (header.user_count > USER_SYNC_MAX_USERS) {
        LOG_WARNING("User sync: NV user count invalid (%d > %d)",
                    header.user_count, USER_SYNC_MAX_USERS);
        return RESULT_INVALID_PARAM;
    }

    /* Read user data */
    user_sync_entry_t temp_users[USER_SYNC_MAX_USERS];
    size_t data_size = (size_t)header.user_count * sizeof(user_sync_entry_t);

    if (data_size > 0) {
        if (g_nv_ops->read(sizeof(header), temp_users, data_size) != 0) {
            LOG_WARNING("User sync: Failed to read NV user data");
            return RESULT_IO_ERROR;
        }
    }

    /* Validate checksum */
    uint16_t computed = compute_nv_checksum(temp_users, header.user_count);
    if (computed != header.checksum) {
        LOG_WARNING("User sync: NV checksum mismatch (0x%04X != 0x%04X)",
                    computed, header.checksum);
        return RESULT_ERROR;
    }

    /* Copy to active storage */
    memset(g_users, 0, sizeof(g_users));
    memcpy(g_users, temp_users, data_size);

    /* Mark as having received sync (from NV is equivalent) */
    g_received_initial_sync = true;

    int count = 0;
    for (int i = 0; i < USER_SYNC_MAX_USERS; i++) {
        if (g_users[i].valid) count++;
    }

    LOG_INFO("User sync: Loaded %d users from NV storage", count);
    return RESULT_OK;
}

result_t user_sync_save_to_nv(void) {
    if (!g_initialized) {
        return RESULT_NOT_INITIALIZED;
    }

    if (!g_nv_ops || !g_nv_ops->write) {
        LOG_DEBUG("User sync: No NV backend, skipping save");
        return RESULT_OK;  /* Not an error - just no persistence */
    }

    /* Count valid users */
    int count = 0;
    for (int i = 0; i < USER_SYNC_MAX_USERS; i++) {
        if (g_users[i].valid) count++;
    }

    /* Build header */
    user_sync_nv_header_t header = {
        .magic = USER_SYNC_NV_MAGIC,
        .version = USER_SYNC_NV_VERSION,
        .user_count = (uint8_t)count,
        .checksum = compute_nv_checksum(g_users, USER_SYNC_MAX_USERS)
    };

    /* Write header */
    if (g_nv_ops->write(0, &header, sizeof(header)) != 0) {
        LOG_ERROR("User sync: Failed to write NV header");
        return RESULT_IO_ERROR;
    }

    /* Write user data */
    size_t data_size = sizeof(g_users);
    if (g_nv_ops->write(sizeof(header), g_users, data_size) != 0) {
        LOG_ERROR("User sync: Failed to write NV user data");
        return RESULT_IO_ERROR;
    }

    /* Flush if available */
    if (g_nv_ops->flush) {
        g_nv_ops->flush();
    }

    LOG_INFO("User sync: Saved %d users to NV storage", count);
    return RESULT_OK;
}

/* ============================================================================
 * Hash Verification Functions
 * ============================================================================ */

void user_sync_hash_with_salt(const char *password,
                               uint32_t *salt_hash,
                               uint32_t *pass_hash) {
    if (salt_hash) {
        *salt_hash = user_sync_djb2_hash(USER_SYNC_SALT);
    }

    if (pass_hash && password) {
        char combined[256];
        snprintf(combined, sizeof(combined), "%s%s", USER_SYNC_SALT, password);
        *pass_hash = user_sync_djb2_hash(combined);
    } else if (pass_hash) {
        *pass_hash = 0;
    }
}

bool user_sync_verify_hash_implementation(void) {
    /*
     * Verified DJB2 test vectors (confirmed with Controller team 2026-01-20):
     *
     * DJB2("")              = 0x00001505 (5381 decimal)
     * DJB2("a")             = 0x0002B606 (177670 decimal)
     * DJB2("NaCl4Life")     = 0x1A3C1FD7 (salt)
     * DJB2("NaCl4Lifetest123") = 0xF82B0BED (salt+password)
     *
     * Test credential:
     *   Username: test_user
     *   Password: test123
     *   Wire format: "DJB2:1A3C1FD7:F82B0BED"
     */
    bool pass = true;

    /* Test empty string */
    if (user_sync_djb2_hash("") != 5381) {
        LOG_ERROR("Hash verify: empty string failed");
        pass = false;
    }

    /* Test single char */
    if (user_sync_djb2_hash("a") != 177670) {
        LOG_ERROR("Hash verify: single char failed");
        pass = false;
    }

    /* Test salt hash - must match controller exactly */
    uint32_t salt_hash = user_sync_djb2_hash(USER_SYNC_SALT);
    if (salt_hash != 0x1A3C1FD7) {
        LOG_ERROR("Hash verify: salt hash failed (got 0x%08X, expected 0x1A3C1FD7)", salt_hash);
        pass = false;
    }
    LOG_INFO("Hash verify: DJB2(\"%s\") = 0x%08X", USER_SYNC_SALT, salt_hash);

    /* Test password hash - verified with controller team */
    uint32_t pass_hash;
    user_sync_hash_with_salt("test123", NULL, &pass_hash);
    if (pass_hash != 0xF82B0BED) {
        LOG_ERROR("Hash verify: password hash failed (got 0x%08X, expected 0xF82B0BED)", pass_hash);
        pass = false;
    }

    /* Verify full wire format */
    char hash_str[USER_SYNC_MAX_HASH];
    user_sync_hash_password("test123", hash_str);
    LOG_INFO("Hash verify: test password hash = %s", hash_str);

    /* Check format matches expected wire format exactly */
    if (strcmp(hash_str, "DJB2:1A3C1FD7:F82B0BED") != 0) {
        LOG_ERROR("Hash verify: wire format mismatch (got %s)", hash_str);
        pass = false;
    }

    if (pass) {
        LOG_INFO("Hash verify: PASSED - RTU/Controller hash algorithms match");
    } else {
        LOG_ERROR("Hash verify: FAILED - hash mismatch with controller");
    }

    return pass;
}

bool user_sync_awaiting_initial_sync(void) {
    if (!g_initialized) {
        return true;  /* Not initialized = awaiting */
    }

    /* If we've received a sync (from controller or NV), we're not awaiting */
    if (g_received_initial_sync) {
        return false;
    }

    /* If we have any users, we're not awaiting */
    for (int i = 0; i < USER_SYNC_MAX_USERS; i++) {
        if (g_users[i].valid) {
            return false;
        }
    }

    return true;
}
