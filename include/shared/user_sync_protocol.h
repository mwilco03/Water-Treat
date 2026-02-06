/**
 * @file user_sync_protocol.h
 * @brief Shared user sync protocol definitions for Water-Controller and Water-Treat
 *
 * This header defines the wire protocol for synchronizing user credentials
 * from the SCADA Controller to RTU devices via PROFINET acyclic data.
 *
 * PROTOCOL OVERVIEW:
 * - Controller sends user_sync_payload_t via PROFINET record write to index 0xF840
 * - RTU receives, validates magic/CRC, stores users in non-volatile memory
 * - RTU uses stored credentials for local TUI/HMI authentication
 *
 * HASH FORMAT:
 * - Algorithm: DJB2 (hash = 5381, hash = ((hash << 5) + hash) + c)
 * - Salt: "NaCl4Life" prepended to password before hashing
 * - Wire format: "DJB2:%08X:%08X" (salt_hash:password_hash) - 22 chars + null
 *
 * IMPORTANT: Both Water-Controller and Water-Treat MUST use these definitions
 * to ensure protocol compatibility. Any changes require version bump.
 *
 * RTU INSTALLATION: This file is pulled from Water-Controller main branch.
 * Do not modify locally - changes must go through Water-Controller repo.
 *
 * Copyright (C) 2024-2025
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SHARED_USER_SYNC_PROTOCOL_H
#define SHARED_USER_SYNC_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============== Protocol Constants ============== */

/** Protocol version - increment on breaking changes */
#define USER_SYNC_PROTOCOL_VERSION  2

/** Magic number for packet validation ("USER" in ASCII) */
#define USER_SYNC_MAGIC             0x55534552

/** PROFINET record index for user sync (vendor-specific range 0xF000-0xFFFF) */
#define USER_SYNC_RECORD_INDEX      0xF840

/** Maximum users per sync payload (RTU storage constraint) */
#define USER_SYNC_MAX_USERS         16

/** Username field length including null terminator */
#define USER_SYNC_USERNAME_LEN      32

/** Password hash field length: "DJB2:%08X:%08X" = 22 chars + null + padding */
#define USER_SYNC_HASH_LEN          24

/** Salt string for DJB2 hashing - MUST match on both sides */
#define USER_SYNC_SALT              "NaCl4Life"

/** DJB2 initial hash value */
#define DJB2_INIT                   5381

/* ============== Operation Types ============== */

/** Replace all users with payload contents */
#define USER_SYNC_OP_FULL_SYNC      0x00

/** Add or update specific users (merge) */
#define USER_SYNC_OP_ADD_UPDATE     0x01

/** Delete specific users by user_id */
#define USER_SYNC_OP_DELETE         0x02

/* ============== User Roles ============== */

/**
 * @brief User role levels for access control
 *
 * Roles are hierarchical: higher values have more permissions.
 * RTU enforces these for local TUI/HMI access.
 */
#ifndef USER_ROLE_VIEWER
#define USER_ROLE_VIEWER    0   /**< Read-only access to status/alarms */
#define USER_ROLE_OPERATOR  1   /**< Can acknowledge alarms, basic control */
#define USER_ROLE_ENGINEER  2   /**< Can modify setpoints, tuning */
#define USER_ROLE_ADMIN     3   /**< Full access including user management */
#endif

/* Typedef for RTU code that doesn't have types.h */
#ifndef WTC_TYPES_H
typedef uint8_t user_sync_role_t;
#else
typedef user_role_t user_sync_role_t;
#endif

/* ============== User Record Flags ============== */

/** User account is active and can authenticate */
#define USER_FLAG_ACTIVE            0x01

/** User should be synced to RTUs (controller-side flag) */
#define USER_FLAG_SYNC_TO_RTUS      0x02

/* ============== Wire Format Structures ============== */

/**
 * @brief User record for PROFINET transfer
 *
 * Fixed-size structure for wire serialization.
 * All fields are packed with no padding.
 * Total size: 64 bytes per user
 */
typedef struct __attribute__((packed)) {
    /** Unique user ID from controller database (for updates/deletes) */
    uint32_t user_id;

    /** Username (null-terminated, max 31 chars + null) */
    char username[USER_SYNC_USERNAME_LEN];

    /** Password hash in format "DJB2:%08X:%08X" (23 chars + null) */
    char password_hash[USER_SYNC_HASH_LEN];

    /** User role (USER_ROLE_* value) */
    uint8_t role;

    /** Flags (USER_FLAG_ACTIVE, USER_FLAG_SYNC_TO_RTUS) */
    uint8_t flags;

    /** Reserved for future use (alignment padding) */
    uint8_t reserved[2];
} user_sync_record_t;

/**
 * @brief Sync payload header
 *
 * Sent before user records. Contains metadata for validation.
 * Total size: 20 bytes
 */
typedef struct __attribute__((packed)) {
    /** Magic number for packet validation (USER_SYNC_MAGIC) */
    uint32_t magic;

    /** Protocol version (USER_SYNC_PROTOCOL_VERSION) */
    uint8_t version;

    /** Operation type (USER_SYNC_OP_*) */
    uint8_t operation;

    /** Number of user records following (0 to USER_SYNC_MAX_USERS) */
    uint8_t user_count;

    /** Reserved for alignment */
    uint8_t reserved;

    /** Unix timestamp when sync was initiated (seconds since epoch) */
    uint32_t timestamp;

    /** Random nonce for replay detection (RTU tracks last seen) */
    uint32_t nonce;

    /** CRC16-CCITT of user records (calculated over user data only) */
    uint16_t checksum;

    /** Reserved for future use */
    uint16_t reserved2;
} user_sync_header_t;

/**
 * @brief Complete sync payload structure
 *
 * Header (20 bytes) + Records (64 bytes * 16) = 1044 bytes max
 * Fits within PROFINET acyclic data limits.
 */
typedef struct __attribute__((packed)) {
    user_sync_header_t header;
    user_sync_record_t users[USER_SYNC_MAX_USERS];
} user_sync_payload_t;

/* ============== Result Codes ============== */

/**
 * @brief User sync operation result codes
 *
 * Negative values indicate errors.
 */
typedef enum {
    USER_SYNC_OK                    =  0,  /**< Operation successful */
    USER_SYNC_ERR_INVALID_PARAM     = -1,  /**< NULL pointer or invalid argument */
    USER_SYNC_ERR_VERSION_MISMATCH  = -2,  /**< Protocol version not supported */
    USER_SYNC_ERR_CHECKSUM          = -3,  /**< CRC validation failed */
    USER_SYNC_ERR_REPLAY            = -4,  /**< Nonce indicates replay attack */
    USER_SYNC_ERR_STORAGE_FULL      = -5,  /**< No room in NV storage */
    USER_SYNC_ERR_STORAGE_WRITE     = -6,  /**< Failed to persist to NV memory */
    USER_SYNC_ERR_USER_NOT_FOUND    = -7,  /**< Username not in storage */
    USER_SYNC_ERR_AUTH_FAILED       = -8,  /**< Password hash mismatch */
    USER_SYNC_ERR_INACTIVE          = -9,  /**< User account is disabled */
    USER_SYNC_ERR_INSUFFICIENT_ROLE = -10, /**< Role below required level */
    USER_SYNC_ERR_BAD_MAGIC         = -11, /**< Magic number mismatch */
    USER_SYNC_ERR_BAD_OPERATION     = -12, /**< Unknown operation type */
} user_sync_result_t;

/* ============== Hash Functions ============== */

/**
 * @brief Compute DJB2 hash of a string
 *
 * Standard DJB2 algorithm with 32-bit overflow.
 * hash = 5381
 * for each char c: hash = ((hash << 5) + hash) + c
 *
 * @param str   Null-terminated string to hash
 * @return      32-bit hash value
 */
static inline uint32_t user_sync_djb2(const char *str) {
    uint32_t hash = DJB2_INIT;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + (uint32_t)c;
    }
    return hash;
}

/**
 * @brief Compute salted DJB2 hash of password
 *
 * Computes: DJB2(salt) and DJB2(salt + password)
 * Both hashes are needed for the wire format.
 *
 * @param password      Plain text password
 * @param salt_hash_out Output: hash of salt alone
 * @param pass_hash_out Output: hash of salt+password
 */
static inline void user_sync_hash_with_salt(const char *password,
                                             uint32_t *salt_hash_out,
                                             uint32_t *pass_hash_out) {
    /* Hash the salt */
    uint32_t salt_hash = user_sync_djb2(USER_SYNC_SALT);
    if (salt_hash_out) {
        *salt_hash_out = salt_hash;
    }

    /* Continue hashing with password (salt already incorporated) */
    uint32_t hash = salt_hash;
    int c;
    const char *p = password;
    while ((c = (unsigned char)*p++)) {
        hash = ((hash << 5) + hash) + (uint32_t)c;
    }
    if (pass_hash_out) {
        *pass_hash_out = hash;
    }
}

/**
 * @brief Format password hash string for wire transfer
 *
 * Generates "DJB2:%08X:%08X" format string.
 *
 * @param password  Plain text password
 * @param hash_out  Output buffer (min USER_SYNC_HASH_LEN bytes)
 */
static inline void user_sync_format_hash(const char *password, char *hash_out) {
    uint32_t salt_hash, pass_hash;
    user_sync_hash_with_salt(password, &salt_hash, &pass_hash);
    /* snprintf not available everywhere, use simple formatting */
    const char hex[] = "0123456789ABCDEF";
    hash_out[0] = 'D'; hash_out[1] = 'J'; hash_out[2] = 'B';
    hash_out[3] = '2'; hash_out[4] = ':';
    for (int i = 0; i < 8; i++) {
        hash_out[5 + i] = hex[(salt_hash >> (28 - i*4)) & 0xF];
    }
    hash_out[13] = ':';
    for (int i = 0; i < 8; i++) {
        hash_out[14 + i] = hex[(pass_hash >> (28 - i*4)) & 0xF];
    }
    hash_out[22] = '\0';
}

/**
 * @brief Constant-time string comparison for hash verification
 *
 * Prevents timing attacks by always comparing all bytes.
 *
 * @param a     First string
 * @param b     Second string
 * @param len   Maximum length to compare
 * @return      true if strings match, false otherwise
 */
static inline bool user_sync_constant_time_compare(const char *a,
                                                    const char *b,
                                                    size_t len) {
    volatile uint8_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= (uint8_t)(a[i] ^ b[i]);
        if (a[i] == '\0' || b[i] == '\0') break;
    }
    return result == 0;
}

/* ============== CRC16-CCITT ============== */

/**
 * @brief Compute CRC16-CCITT checksum
 *
 * Polynomial: 0x1021 (x^16 + x^12 + x^5 + 1)
 * Initial value: 0xFFFF
 * Used for payload integrity verification.
 *
 * @param data  Data buffer
 * @param len   Data length in bytes
 * @return      16-bit CRC value
 */
static inline uint16_t user_sync_crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
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

/* ============== Validation Functions ============== */

/**
 * @brief Validate sync payload header
 *
 * Checks magic number, version, and basic sanity.
 *
 * @param header  Header to validate
 * @return        USER_SYNC_OK or error code
 */
static inline user_sync_result_t user_sync_validate_header(
    const user_sync_header_t *header) {
    if (!header) {
        return USER_SYNC_ERR_INVALID_PARAM;
    }
    if (header->magic != USER_SYNC_MAGIC) {
        return USER_SYNC_ERR_BAD_MAGIC;
    }
    if (header->version != USER_SYNC_PROTOCOL_VERSION) {
        return USER_SYNC_ERR_VERSION_MISMATCH;
    }
    if (header->operation > USER_SYNC_OP_DELETE) {
        return USER_SYNC_ERR_BAD_OPERATION;
    }
    if (header->user_count > USER_SYNC_MAX_USERS) {
        return USER_SYNC_ERR_INVALID_PARAM;
    }
    return USER_SYNC_OK;
}

/**
 * @brief Validate complete payload including CRC
 *
 * @param payload  Payload to validate
 * @return         USER_SYNC_OK or error code
 */
static inline user_sync_result_t user_sync_validate_payload(
    const user_sync_payload_t *payload) {
    user_sync_result_t result = user_sync_validate_header(&payload->header);
    if (result != USER_SYNC_OK) {
        return result;
    }

    /* Verify CRC over user records */
    size_t data_len = payload->header.user_count * sizeof(user_sync_record_t);
    uint16_t expected_crc = user_sync_crc16_ccitt(
        (const uint8_t *)payload->users, data_len);
    if (payload->header.checksum != expected_crc) {
        return USER_SYNC_ERR_CHECKSUM;
    }

    return USER_SYNC_OK;
}

/**
 * @brief Initialize payload header with defaults
 *
 * @param header      Header to initialize
 * @param operation   Operation type (USER_SYNC_OP_*)
 * @param user_count  Number of users
 * @param timestamp   Unix timestamp (0 for current time if available)
 */
static inline void user_sync_init_header(user_sync_header_t *header,
                                          uint8_t operation,
                                          uint8_t user_count,
                                          uint32_t timestamp) {
    memset(header, 0, sizeof(*header));
    header->magic = USER_SYNC_MAGIC;
    header->version = USER_SYNC_PROTOCOL_VERSION;
    header->operation = operation;
    header->user_count = user_count;
    header->timestamp = timestamp;
    /* nonce and checksum set by caller */
}

/* ============== Utility Functions ============== */

/**
 * @brief Get string representation of result code
 *
 * @param result  Result code
 * @return        Human-readable string
 */
static inline const char *user_sync_result_str(user_sync_result_t result) {
    switch (result) {
        case USER_SYNC_OK:                    return "OK";
        case USER_SYNC_ERR_INVALID_PARAM:     return "Invalid parameter";
        case USER_SYNC_ERR_VERSION_MISMATCH:  return "Version mismatch";
        case USER_SYNC_ERR_CHECKSUM:          return "Checksum error";
        case USER_SYNC_ERR_REPLAY:            return "Replay detected";
        case USER_SYNC_ERR_STORAGE_FULL:      return "Storage full";
        case USER_SYNC_ERR_STORAGE_WRITE:     return "Storage write failed";
        case USER_SYNC_ERR_USER_NOT_FOUND:    return "User not found";
        case USER_SYNC_ERR_AUTH_FAILED:       return "Authentication failed";
        case USER_SYNC_ERR_INACTIVE:          return "User inactive";
        case USER_SYNC_ERR_INSUFFICIENT_ROLE: return "Insufficient role";
        case USER_SYNC_ERR_BAD_MAGIC:         return "Bad magic number";
        case USER_SYNC_ERR_BAD_OPERATION:     return "Bad operation type";
        default:                               return "Unknown error";
    }
}

/**
 * @brief Get string representation of user role
 *
 * @param role  User role value
 * @return      Human-readable string
 */
static inline const char *user_sync_role_str(user_sync_role_t role) {
    switch (role) {
        case USER_ROLE_VIEWER:   return "Viewer";
        case USER_ROLE_OPERATOR: return "Operator";
        case USER_ROLE_ENGINEER: return "Engineer";
        case USER_ROLE_ADMIN:    return "Admin";
        default:                  return "Unknown";
    }
}

/**
 * @brief Get string representation of operation type
 *
 * @param op  Operation type
 * @return    Human-readable string
 */
static inline const char *user_sync_op_str(uint8_t op) {
    switch (op) {
        case USER_SYNC_OP_FULL_SYNC:  return "Full Sync";
        case USER_SYNC_OP_ADD_UPDATE: return "Add/Update";
        case USER_SYNC_OP_DELETE:     return "Delete";
        default:                       return "Unknown";
    }
}

/**
 * @brief Check if role meets minimum requirement
 *
 * @param user_role      User's actual role
 * @param required_role  Minimum required role
 * @return               true if user_role >= required_role
 */
static inline bool user_sync_role_sufficient(user_sync_role_t user_role,
                                              user_sync_role_t required_role) {
    return (int)user_role >= (int)required_role;
}

/**
 * @brief Calculate payload size for given user count
 *
 * @param user_count  Number of users (0 to USER_SYNC_MAX_USERS)
 * @return            Total payload size in bytes
 */
static inline size_t user_sync_payload_size(uint8_t user_count) {
    return sizeof(user_sync_header_t) +
           ((size_t)user_count * sizeof(user_sync_record_t));
}

#ifdef __cplusplus
}
#endif

#endif /* SHARED_USER_SYNC_PROTOCOL_H */
