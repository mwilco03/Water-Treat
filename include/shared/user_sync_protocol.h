/**
 * @file user_sync_protocol.h
 * @brief Shared protocol definitions for user credential synchronization
 *
 * CANONICAL SOURCE: This file defines the wire format for user credential
 * sync between the SCADA Controller and RTU devices via PROFINET acyclic
 * record writes (index 0xF840).
 *
 * Both Water-Controller and Water-Treat (RTU) must use identical definitions.
 * The Water-Controller repository is the upstream source of truth.
 * RTU fetches this header via scripts/fetch_shared_protocols.sh.
 *
 * Wire format: All multi-byte integers are in network byte order (big-endian).
 * Structs are packed (__attribute__((packed))) — no padding between fields.
 *
 * Hash format: "DJB2:%08X:%08X" where first field is DJB2(salt), second is
 * DJB2(password). Salt is USER_SYNC_SALT ("NaCl4Life").
 */

#ifndef USER_SYNC_PROTOCOL_H
#define USER_SYNC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Protocol Constants
 * ========================================================================== */

/** Protocol version — increment on breaking wire format changes */
#define USER_SYNC_PROTOCOL_VERSION  1

/** Magic number for packet identification: "USYN" in big-endian */
#define USER_SYNC_MAGIC             0x5553594E

/** PROFINET record index for user sync writes */
#define USER_SYNC_RECORD_INDEX      0xF840

/** Maximum users per sync (embedded constraint — static allocation) */
#define USER_SYNC_MAX_USERS         16

/** Maximum username length including null terminator */
#define USER_SYNC_USERNAME_LEN      64

/** Maximum hash string length including null terminator.
 *  Format: "DJB2:%08X:%08X" = 22 chars + null = 23, padded to 32. */
#define USER_SYNC_HASH_LEN          32

/** Salt for DJB2 hash computation */
#define USER_SYNC_SALT              "NaCl4Life"

/** DJB2 hash initial value */
#define DJB2_INIT                   ((uint32_t)5381)

/* ============================================================================
 * Operation Codes
 * ========================================================================== */

/** Replace all RTU users with this set */
#define USER_SYNC_OP_FULL_SYNC      0

/** Add or update individual users */
#define USER_SYNC_OP_ADD_UPDATE     1

/** Delete individual users */
#define USER_SYNC_OP_DELETE          2

/* ============================================================================
 * User Roles
 * ========================================================================== */

/** Role type — uint8_t on the wire */
typedef uint8_t user_sync_role_t;

#define USER_ROLE_VIEWER            ((user_sync_role_t)0)
#define USER_ROLE_OPERATOR          ((user_sync_role_t)1)
#define USER_ROLE_ENGINEER          ((user_sync_role_t)2)
#define USER_ROLE_ADMIN             ((user_sync_role_t)3)

/* ============================================================================
 * User Flags (bitfield)
 * ========================================================================== */

/** Account is enabled */
#define USER_FLAG_ACTIVE            0x01

/** Controller marked this user for RTU synchronization */
#define USER_FLAG_SYNC_TO_RTUS      0x02

/* ============================================================================
 * Result Codes
 * ========================================================================== */

typedef enum {
    USER_SYNC_OK = 0,
    USER_SYNC_ERR_MAGIC,        /**< Invalid magic number */
    USER_SYNC_ERR_VERSION,      /**< Unsupported protocol version */
    USER_SYNC_ERR_SIZE,         /**< Packet too short for declared content */
    USER_SYNC_ERR_CHECKSUM,     /**< CRC16 mismatch */
    USER_SYNC_ERR_OPERATION,    /**< Unknown operation code */
    USER_SYNC_ERR_COUNT,        /**< User count exceeds MAX_USERS */
} user_sync_result_t;

/* ============================================================================
 * Wire Format Structures (packed, network byte order)
 * ========================================================================== */

/**
 * Sync packet header (12 bytes)
 *
 * Byte layout:
 *   [0..3]  magic        uint32_t  USER_SYNC_MAGIC
 *   [4]     version      uint8_t   USER_SYNC_PROTOCOL_VERSION
 *   [5]     operation    uint8_t   USER_SYNC_OP_*
 *   [6]     user_count   uint8_t   Number of records following
 *   [7]     reserved     uint8_t   Must be 0
 *   [8..11] timestamp    uint32_t  Epoch seconds (network byte order)
 *   [12..13] checksum    uint16_t  CRC16-CCITT of payload (network byte order)
 *   [14..15] reserved2   uint16_t  Must be 0
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  operation;
    uint8_t  user_count;
    uint8_t  reserved;
    uint32_t timestamp;
    uint16_t checksum;
    uint16_t reserved2;
} user_sync_header_t;

/**
 * Per-user record (network byte order where applicable)
 *
 * Byte layout:
 *   [0..3]   user_id        uint32_t  Unique ID (network byte order)
 *   [4..67]  username       char[64]  Null-terminated UTF-8
 *   [68..99] password_hash  char[32]  "DJB2:%08X:%08X" null-terminated
 *   [100]    role           uint8_t   USER_ROLE_*
 *   [101]    flags          uint8_t   USER_FLAG_* bitfield
 *   [102..103] reserved     uint16_t  Must be 0
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
 * Full sync payload (header + variable number of records)
 */
typedef struct __attribute__((packed)) {
    user_sync_header_t header;
    user_sync_record_t records[];
} user_sync_payload_t;

/* ============================================================================
 * Inline Functions
 * ========================================================================== */

/**
 * DJB2 hash of a null-terminated string
 * Matches controller's _djb2_hash() with 32-bit overflow masking.
 */
static inline uint32_t user_sync_djb2(const char *str) {
    uint32_t hash = DJB2_INIT;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + (uint32_t)c;  /* hash * 33 + c */
    }
    return hash;
}

/**
 * Compute DJB2 hash of password (with optional salt override).
 *
 * @param password  Password to hash
 * @param salt      Salt string (NULL to use default USER_SYNC_SALT)
 * @param out_hash  Receives the 32-bit hash result
 */
static inline void user_sync_hash_with_salt(const char *password,
                                              const char *salt,
                                              uint32_t *out_hash) {
    (void)salt;  /* Salt is hashed separately in the wire format */
    *out_hash = user_sync_djb2(password);
}

/**
 * Format password hash in controller-compatible format.
 * Output: "DJB2:<salt_hash>:<password_hash>"
 *
 * @param password   Plaintext password
 * @param out        Buffer of at least USER_SYNC_HASH_LEN bytes
 */
static inline void user_sync_format_hash(const char *password, char *out) {
    uint32_t salt_hash = user_sync_djb2(USER_SYNC_SALT);
    uint32_t pass_hash = user_sync_djb2(password);
    snprintf(out, USER_SYNC_HASH_LEN, "DJB2:%08X:%08X", salt_hash, pass_hash);
}

/**
 * Constant-time comparison of two buffers.
 * Prevents timing attacks on hash comparison.
 *
 * @return true if first `len` bytes are equal
 */
static inline bool user_sync_constant_time_compare(const char *a, const char *b,
                                                     size_t len) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)((unsigned char)a[i] ^ (unsigned char)b[i]);
    }
    return diff == 0;
}

/**
 * CRC16-CCITT (polynomial 0x1021, init 0xFFFF)
 *
 * Used for packet integrity verification.
 */
static inline uint16_t user_sync_crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
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

/**
 * Validate sync packet header (magic, version, operation, user count).
 *
 * @return USER_SYNC_OK if valid, error code otherwise
 */
static inline user_sync_result_t user_sync_validate_header(const user_sync_header_t *hdr) {
    if (!hdr) return USER_SYNC_ERR_SIZE;

    if (hdr->magic != USER_SYNC_MAGIC) {
        /* Try network byte order */
        uint32_t magic_n = ((hdr->magic >> 24) & 0xFF) |
                           ((hdr->magic >> 8) & 0xFF00) |
                           ((hdr->magic << 8) & 0xFF0000) |
                           ((hdr->magic << 24) & 0xFF000000);
        if (magic_n != USER_SYNC_MAGIC) {
            return USER_SYNC_ERR_MAGIC;
        }
    }

    if (hdr->version != USER_SYNC_PROTOCOL_VERSION) {
        return USER_SYNC_ERR_VERSION;
    }

    if (hdr->operation > USER_SYNC_OP_DELETE) {
        return USER_SYNC_ERR_OPERATION;
    }

    if (hdr->user_count > USER_SYNC_MAX_USERS) {
        return USER_SYNC_ERR_COUNT;
    }

    return USER_SYNC_OK;
}

/**
 * Validate full payload (header + size check for records).
 *
 * @param data      Raw packet data
 * @param length    Total packet length
 * @return USER_SYNC_OK if valid
 */
static inline user_sync_result_t user_sync_validate_payload(const uint8_t *data,
                                                              size_t length) {
    if (!data || length < sizeof(user_sync_header_t)) {
        return USER_SYNC_ERR_SIZE;
    }

    const user_sync_header_t *hdr = (const user_sync_header_t *)data;
    user_sync_result_t result = user_sync_validate_header(hdr);
    if (result != USER_SYNC_OK) {
        return result;
    }

    size_t expected = sizeof(user_sync_header_t) +
                      (size_t)hdr->user_count * sizeof(user_sync_record_t);
    if (length < expected) {
        return USER_SYNC_ERR_SIZE;
    }

    return USER_SYNC_OK;
}

/**
 * Initialize a sync header with default values.
 */
static inline void user_sync_init_header(user_sync_header_t *hdr,
                                           uint8_t operation,
                                           uint8_t user_count) {
    if (!hdr) return;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = USER_SYNC_MAGIC;
    hdr->version = USER_SYNC_PROTOCOL_VERSION;
    hdr->operation = operation;
    hdr->user_count = user_count;
}

/**
 * Compute total payload size for a given user count.
 */
static inline size_t user_sync_payload_size(uint8_t user_count) {
    return sizeof(user_sync_header_t) +
           (size_t)user_count * sizeof(user_sync_record_t);
}

/* ============================================================================
 * String Conversion Functions
 * ========================================================================== */

/** Result code to human-readable string */
static inline const char* user_sync_result_str(user_sync_result_t result) {
    switch (result) {
        case USER_SYNC_OK:            return "OK";
        case USER_SYNC_ERR_MAGIC:     return "invalid magic";
        case USER_SYNC_ERR_VERSION:   return "unsupported version";
        case USER_SYNC_ERR_SIZE:      return "packet too short";
        case USER_SYNC_ERR_CHECKSUM:  return "checksum mismatch";
        case USER_SYNC_ERR_OPERATION: return "unknown operation";
        case USER_SYNC_ERR_COUNT:     return "user count exceeds max";
        default:                      return "unknown error";
    }
}

/** Role to human-readable string */
static inline const char* user_sync_role_str(user_sync_role_t role) {
    switch (role) {
        case USER_ROLE_VIEWER:   return "viewer";
        case USER_ROLE_OPERATOR: return "operator";
        case USER_ROLE_ENGINEER: return "engineer";
        case USER_ROLE_ADMIN:    return "admin";
        default:                 return "unknown";
    }
}

/** Operation code to human-readable string */
static inline const char* user_sync_op_str(uint8_t operation) {
    switch (operation) {
        case USER_SYNC_OP_FULL_SYNC:   return "FULL_SYNC";
        case USER_SYNC_OP_ADD_UPDATE:  return "ADD_UPDATE";
        case USER_SYNC_OP_DELETE:      return "DELETE";
        default:                       return "UNKNOWN";
    }
}

/**
 * Check if a role meets or exceeds the required level.
 *
 * @param user_role      Role of the user
 * @param required_role  Minimum required role
 * @return true if user_role >= required_role
 */
static inline bool user_sync_role_sufficient(user_sync_role_t user_role,
                                               user_sync_role_t required_role) {
    return user_role >= required_role;
}

#endif /* USER_SYNC_PROTOCOL_H */
