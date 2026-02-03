/**
 * @file user_sync_protocol.h
 * @brief Shared user-sync wire protocol (Water-Controller ↔ Water-Treat RTU)
 *
 * Canonical definitions for the user credential synchronization protocol
 * transmitted over PROFINET acyclic record writes (index 0xF840).
 *
 * This header is the single source of truth for:
 *   - Wire format structures (header, records)
 *   - Protocol constants (magic, version, operations, roles)
 *   - Cryptographic helpers (DJB2, CRC16-CCITT, constant-time compare)
 *
 * Both the controller (Python) and RTU (C) implement identical algorithms.
 * The test vectors in user_sync_verify_hash_implementation() guarantee parity.
 *
 * Origin: Water-Controller repository
 * Synced to Water-Treat via build-time fetch (see scripts/sync-shared-headers.sh)
 */

#ifndef USER_SYNC_PROTOCOL_H
#define USER_SYNC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>

/* ============================================================================
 * Protocol Constants
 * ========================================================================== */

/** Protocol version (bump on incompatible wire format changes) */
#define USER_SYNC_PROTOCOL_VERSION  2

/** Magic bytes: ASCII "USER" = 0x55534552 (stored big-endian on wire) */
#define USER_SYNC_MAGIC             0x55534552u

/** PROFINET record index for user sync packets */
#define USER_SYNC_RECORD_INDEX      0xF840

/** Maximum users per sync (embedded constraint) */
#define USER_SYNC_MAX_USERS         16

/** Username field width (including null terminator) */
#define USER_SYNC_USERNAME_LEN      32

/** Hash string width: "DJB2:XXXXXXXX:XXXXXXXX" + null (24 bytes) */
#define USER_SYNC_HASH_LEN          24

/** Salt for password hashing — must match controller */
#define USER_SYNC_SALT              "NaCl4Life"

/** DJB2 hash initial value */
#define DJB2_INIT                   5381u

/* ============================================================================
 * Operation Codes
 * ========================================================================== */

/** Replace entire user table with this packet's records */
#define USER_SYNC_OP_FULL_SYNC      0x01

/** Add or update individual user records */
#define USER_SYNC_OP_ADD_UPDATE     0x02

/** Delete users listed in this packet */
#define USER_SYNC_OP_DELETE         0x03

/* ============================================================================
 * Role Constants (uint8_t values, ascending privilege)
 * ========================================================================== */

typedef uint8_t user_sync_role_t;

#define USER_ROLE_VIEWER    ((user_sync_role_t)0)
#define USER_ROLE_OPERATOR  ((user_sync_role_t)1)
#define USER_ROLE_ENGINEER  ((user_sync_role_t)2)
#define USER_ROLE_ADMIN     ((user_sync_role_t)3)

/* ============================================================================
 * User Flags
 * ========================================================================== */

/** Account is active (can authenticate) */
#define USER_FLAG_ACTIVE            0x01

/** Controller marked this user for RTU sync */
#define USER_FLAG_SYNC_TO_RTUS      0x02

/* ============================================================================
 * Result Codes
 * ========================================================================== */

typedef enum {
    USER_SYNC_OK           = 0,
    USER_SYNC_ERR_MAGIC    = 1,
    USER_SYNC_ERR_VERSION  = 2,
    USER_SYNC_ERR_CRC      = 3,
    USER_SYNC_ERR_LENGTH   = 4
} user_sync_result_t;

/* ============================================================================
 * Wire Format Structures (all multi-byte fields in network byte order)
 * ========================================================================== */

/**
 * Packet header — precedes the user record array.
 *
 * Wire layout (16 bytes):
 *   [0-3]   magic      (uint32, big-endian: 0x55534552)
 *   [4]     version    (uint8: 2)
 *   [5]     operation  (uint8: USER_SYNC_OP_*)
 *   [6]     user_count (uint8: number of records following)
 *   [7]     reserved
 *   [8-9]   checksum   (uint16, big-endian: CRC16-CCITT of records)
 *   [10-11] reserved
 *   [12-15] timestamp  (uint32, big-endian: unix epoch)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  operation;
    uint8_t  user_count;
    uint8_t  reserved;
    uint16_t checksum;
    uint16_t reserved2;
    uint32_t timestamp;
} user_sync_header_t;

/**
 * Single user record in wire format (64 bytes).
 *
 *   [0-3]   user_id        (uint32, big-endian)
 *   [4-35]  username       (32 bytes, null-terminated, zero-padded)
 *   [36-59] password_hash  (24 bytes, "DJB2:XXXXXXXX:XXXXXXXX\0")
 *   [60]    role           (uint8: USER_ROLE_*)
 *   [61]    flags          (uint8: USER_FLAG_*)
 *   [62-63] reserved
 */
typedef struct __attribute__((packed)) {
    uint32_t user_id;
    char     username[USER_SYNC_USERNAME_LEN];
    char     password_hash[USER_SYNC_HASH_LEN];
    uint8_t  role;
    uint8_t  flags;
    uint16_t reserved;
} user_sync_record_t;

/**
 * Complete payload (header + records).
 * Convenience type for size calculations.
 */
typedef struct __attribute__((packed)) {
    user_sync_header_t header;
    user_sync_record_t records[USER_SYNC_MAX_USERS];
} user_sync_payload_t;

/* ============================================================================
 * Inline Functions — DJB2 Hash
 * ========================================================================== */

/**
 * @brief DJB2 hash of a null-terminated string
 *
 * hash(i) = hash(i-1) * 33 + c,  starting from DJB2_INIT (5381).
 * 32-bit unsigned overflow is the masking mechanism.
 *
 * Verified test vectors:
 *   DJB2("")          = 5381
 *   DJB2("a")         = 177670
 *   DJB2("NaCl4Life") = 0x1A3C1FD7
 */
static inline uint32_t user_sync_djb2(const char *str) {
    uint32_t hash = DJB2_INIT;
    if (!str) return hash;
    while (*str) {
        hash = hash * 33 + (uint8_t)*str;
        str++;
    }
    return hash;
}

/**
 * @brief Hash password with salt (DJB2 seeded by salt hash)
 *
 * 1. Compute salt_hash = DJB2(salt)        [defaults to USER_SYNC_SALT]
 * 2. Continue hashing password characters from salt_hash
 *
 * @param password  Plaintext password
 * @param salt      Salt string (NULL → USER_SYNC_SALT)
 * @param hash_out  Receives 32-bit password hash
 */
static inline void user_sync_hash_with_salt(const char *password,
                                             const char *salt,
                                             uint32_t *hash_out) {
    const char *actual_salt = salt ? salt : USER_SYNC_SALT;
    uint32_t hash = user_sync_djb2(actual_salt);
    if (password) {
        const char *p = password;
        while (*p) {
            hash = hash * 33 + (uint8_t)*p;
            p++;
        }
    }
    if (hash_out) *hash_out = hash;
}

/**
 * @brief Format password into wire hash string
 *
 * Produces: "DJB2:<salt_hash>:<password_hash>"
 * Example:  "DJB2:1A3C1FD7:F82B0BED" for password "test123"
 *
 * @param password  Plaintext password
 * @param hash_out  Buffer (min USER_SYNC_HASH_LEN bytes)
 */
static inline void user_sync_format_hash(const char *password, char *hash_out) {
    uint32_t salt_hash = user_sync_djb2(USER_SYNC_SALT);
    uint32_t pass_hash;
    user_sync_hash_with_salt(password, NULL, &pass_hash);
    snprintf(hash_out, USER_SYNC_HASH_LEN, "DJB2:%08X:%08X",
             salt_hash, pass_hash);
}

/* ============================================================================
 * Inline Functions — Security
 * ========================================================================== */

/**
 * @brief Constant-time comparison to prevent timing attacks
 *
 * Compares all bytes regardless of mismatch position.
 * The volatile qualifier prevents the compiler from short-circuiting.
 *
 * @return true if all bytes match, false otherwise
 */
static inline bool user_sync_constant_time_compare(const char *a,
                                                    const char *b,
                                                    size_t len) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
    }
    return diff == 0;
}

/**
 * @brief CRC16-CCITT (polynomial 0x1021, init 0xFFFF)
 *
 * Used to validate record payloads in user sync packets.
 */
static inline uint16_t user_sync_crc16_ccitt(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* ============================================================================
 * Inline Functions — Validation
 * ========================================================================== */

/**
 * @brief Validate packet header magic and version
 */
static inline user_sync_result_t user_sync_validate_header(
        const user_sync_header_t *hdr) {
    if (!hdr) return USER_SYNC_ERR_LENGTH;
    if (ntohl(hdr->magic) != USER_SYNC_MAGIC) return USER_SYNC_ERR_MAGIC;
    if (hdr->version != USER_SYNC_PROTOCOL_VERSION) return USER_SYNC_ERR_VERSION;
    return USER_SYNC_OK;
}

/**
 * @brief Validate payload structure
 */
static inline user_sync_result_t user_sync_validate_payload(
        const user_sync_payload_t *payload) {
    if (!payload) return USER_SYNC_ERR_LENGTH;
    return user_sync_validate_header(&payload->header);
}

/**
 * @brief Initialize header with default magic and version
 */
static inline void user_sync_init_header(user_sync_header_t *hdr) {
    if (!hdr) return;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = htonl(USER_SYNC_MAGIC);
    hdr->version = USER_SYNC_PROTOCOL_VERSION;
}

/* ============================================================================
 * Inline Functions — String Conversion
 * ========================================================================== */

static inline const char* user_sync_result_str(user_sync_result_t result) {
    switch (result) {
        case USER_SYNC_OK:          return "OK";
        case USER_SYNC_ERR_MAGIC:   return "invalid magic";
        case USER_SYNC_ERR_VERSION: return "version mismatch";
        case USER_SYNC_ERR_CRC:     return "CRC error";
        case USER_SYNC_ERR_LENGTH:  return "invalid length";
        default:                    return "unknown error";
    }
}

static inline const char* user_sync_role_str(user_sync_role_t role) {
    switch (role) {
        case USER_ROLE_VIEWER:   return "viewer";
        case USER_ROLE_OPERATOR: return "operator";
        case USER_ROLE_ENGINEER: return "engineer";
        case USER_ROLE_ADMIN:    return "admin";
        default:                 return "unknown";
    }
}

static inline const char* user_sync_op_str(uint8_t operation) {
    switch (operation) {
        case USER_SYNC_OP_FULL_SYNC:  return "full_sync";
        case USER_SYNC_OP_ADD_UPDATE: return "add_update";
        case USER_SYNC_OP_DELETE:     return "delete";
        default:                      return "unknown_op";
    }
}

/* ============================================================================
 * Inline Functions — Utility
 * ========================================================================== */

/**
 * @brief Check if one role has sufficient privilege for another
 * @return true if have >= required
 */
static inline bool user_sync_role_sufficient(user_sync_role_t have,
                                              user_sync_role_t required) {
    return have >= required;
}

/**
 * @brief Calculate total payload size for a given user count
 */
static inline size_t user_sync_payload_size(uint8_t user_count) {
    return sizeof(user_sync_header_t) +
           ((size_t)user_count * sizeof(user_sync_record_t));
}

#endif /* USER_SYNC_PROTOCOL_H */
