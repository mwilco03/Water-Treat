/**
 * @file rtu_registration.h
 * @brief RTU registration and enrollment with SCADA controller
 *
 * Implements RTU registration protocol for establishing device binding
 * between RTU and SCADA controller. Supports both:
 * 1. HTTP-based registration (POST /api/v1/rtu/register)
 * 2. PROFINET-based enrollment (index 0xF845)
 *
 * Registration Flow:
 * 1. RTU discovers controller via mDNS or config
 * 2. RTU sends registration request with device info
 * 3. Controller validates and returns enrollment token
 * 4. Controller sends binding confirmation via PROFINET 0xF845
 * 5. RTU stores enrollment token for future auth
 *
 * Security Design:
 * - Enrollment tokens are cryptographic (32 hex chars)
 * - Token format: "wtc-enroll-{32 hex}"
 * - Tokens stored in NV storage for persistence
 * - Registration requires network path to controller
 */

#ifndef RTU_REGISTRATION_H
#define RTU_REGISTRATION_H

#include "common.h"
#include "config/config.h"
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

/** PROFINET record index for enrollment/binding */
#define RTU_ENROLL_PROFINET_INDEX       0xF845

/** PROFINET record index for device config */
#define RTU_CONFIG_PROFINET_INDEX       0xF841

/** PROFINET record index for sensor config */
#define RTU_SENSOR_CONFIG_PROFINET_INDEX 0xF842

/** PROFINET record index for actuator config */
#define RTU_ACTUATOR_CONFIG_PROFINET_INDEX 0xF843

/** Enrollment token prefix */
#define RTU_ENROLL_TOKEN_PREFIX         "wtc-enroll-"

/** Enrollment token length (prefix + 32 hex chars + null) */
#define RTU_ENROLL_TOKEN_LEN            44

/** Maximum serial number length */
#define RTU_SERIAL_LEN                  32

/** Maximum MAC address string length (xx:xx:xx:xx:xx:xx + null) */
#define RTU_MAC_ADDR_LEN                18

/** Maximum capabilities string length */
#define RTU_CAPABILITIES_LEN            128

/** Registration retry interval (ms) */
#define RTU_REGISTRATION_RETRY_MS       30000

/** Registration timeout (ms) */
#define RTU_REGISTRATION_TIMEOUT_MS     10000

/** Magic header for enrollment packets */
#define RTU_ENROLL_MAGIC                0x454E524C  /* "ENRL" in ASCII */

/** Protocol version for enrollment */
#define RTU_ENROLL_VERSION              1

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * Registration state machine states
 */
typedef enum {
    RTU_REG_STATE_UNREGISTERED = 0,   /**< Not registered with any controller */
    RTU_REG_STATE_DISCOVERING,         /**< Looking for controller */
    RTU_REG_STATE_REGISTERING,         /**< Registration request sent */
    RTU_REG_STATE_AWAITING_CONFIRM,    /**< Awaiting PROFINET binding */
    RTU_REG_STATE_REGISTERED,          /**< Successfully registered */
    RTU_REG_STATE_ERROR                /**< Registration failed */
} rtu_registration_state_t;

/**
 * Enrollment operation types (via PROFINET 0xF845)
 */
typedef enum {
    RTU_ENROLL_OP_BIND      = 0,    /**< Controller binding this RTU */
    RTU_ENROLL_OP_UNBIND    = 1,    /**< Controller unbinding this RTU */
    RTU_ENROLL_OP_REBIND    = 2,    /**< Force rebind (new token) */
    RTU_ENROLL_OP_STATUS    = 3     /**< Query enrollment status */
} rtu_enroll_operation_t;

/**
 * RTU device information for registration
 */
typedef struct {
    char station_name[MAX_NAME_LEN];      /**< PROFINET station name */
    char serial_number[RTU_SERIAL_LEN];   /**< Device serial number */
    uint16_t vendor_id;                    /**< PROFINET vendor ID */
    uint16_t device_id;                    /**< PROFINET device ID */
    char mac_address[RTU_MAC_ADDR_LEN];   /**< Primary interface MAC */
    char capabilities[RTU_CAPABILITIES_LEN]; /**< Comma-sep capabilities */
    uint8_t sensor_count;                  /**< Number of sensors */
    uint8_t actuator_count;                /**< Number of actuators */
    char firmware_version[32];             /**< Firmware version string */
} rtu_device_info_t;

/**
 * Enrollment packet header (from controller via PROFINET 0xF845)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;                         /**< RTU_ENROLL_MAGIC */
    uint8_t  version;                       /**< Protocol version */
    uint8_t  operation;                     /**< rtu_enroll_operation_t */
    uint16_t reserved;                      /**< Reserved for alignment */
    uint32_t timestamp;                     /**< Enrollment timestamp */
    char     enrollment_token[RTU_ENROLL_TOKEN_LEN]; /**< Token from controller */
    uint16_t checksum;                      /**< CRC16-CCITT of packet */
    uint16_t padding;                       /**< Padding to 4-byte align */
} rtu_enroll_packet_t;

/**
 * Registration status for diagnostics
 */
typedef struct {
    rtu_registration_state_t state;         /**< Current state */
    char controller_ip[16];                 /**< Registered controller IP */
    char controller_name[MAX_NAME_LEN];     /**< Controller station name */
    char enrollment_token[RTU_ENROLL_TOKEN_LEN]; /**< Current token */
    uint64_t registered_at;                 /**< Registration timestamp (ms) */
    uint64_t last_attempt;                  /**< Last registration attempt */
    uint32_t attempt_count;                 /**< Registration attempts */
    uint32_t error_count;                   /**< Total errors */
    int last_http_status;                   /**< Last HTTP response code */
    char last_error[64];                    /**< Last error message */
} rtu_registration_status_t;

/**
 * Callback when registration state changes
 */
typedef void (*rtu_registration_callback_t)(rtu_registration_state_t state,
                                             const rtu_registration_status_t *status,
                                             void *ctx);

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Initialize RTU registration subsystem
 *
 * @param config   Application configuration (for device info)
 * @return RESULT_OK on success
 */
result_t rtu_registration_init(const app_config_t *config);

/**
 * Shutdown RTU registration
 */
void rtu_registration_shutdown(void);

/**
 * Start registration process
 *
 * Begins attempting to register with discovered/configured controller.
 * Runs in background thread with automatic retry.
 *
 * @return RESULT_OK if started
 */
result_t rtu_registration_start(void);

/**
 * Stop registration attempts
 */
void rtu_registration_stop(void);

/**
 * Force immediate registration attempt
 *
 * @param controller_ip   Controller IP to register with (or NULL for discovered)
 * @return RESULT_OK if attempt started
 */
result_t rtu_registration_trigger(const char *controller_ip);

/**
 * Process enrollment packet from PROFINET
 *
 * Called from profinet_write_callback() when enrollment record received.
 *
 * @param data      Raw packet data
 * @param length    Packet length in bytes
 * @return RESULT_OK on success
 */
result_t rtu_registration_process_enrollment(const uint8_t *data, uint16_t length);

/**
 * Get current registration status
 *
 * @param status    Output status structure
 * @return RESULT_OK on success
 */
result_t rtu_registration_get_status(rtu_registration_status_t *status);

/**
 * Check if RTU is registered with a controller
 *
 * @return true if registered and token valid
 */
bool rtu_registration_is_registered(void);

/**
 * Get enrollment token (for auth with controller)
 *
 * @param token     Buffer for token (min RTU_ENROLL_TOKEN_LEN bytes)
 * @return RESULT_OK if token available
 */
result_t rtu_registration_get_token(char *token);

/**
 * Clear registration (unbind from controller)
 *
 * @return RESULT_OK on success
 */
result_t rtu_registration_clear(void);

/**
 * Set callback for state changes
 *
 * @param callback  Function to call on state change
 * @param ctx       User context
 */
void rtu_registration_set_callback(rtu_registration_callback_t callback, void *ctx);

/**
 * Get device info for registration payload
 *
 * @param info      Output device info structure
 * @return RESULT_OK on success
 */
result_t rtu_registration_get_device_info(rtu_device_info_t *info);

/* ============================================================================
 * State String Helpers
 * ============================================================================ */

/**
 * Get registration state as string
 */
const char* rtu_registration_state_to_string(rtu_registration_state_t state);

/**
 * Get enrollment operation as string
 */
const char* rtu_enroll_op_to_string(rtu_enroll_operation_t op);

/* ============================================================================
 * NV Storage Backend Interface
 * ============================================================================ */

/**
 * NV storage operations for enrollment token persistence
 */
typedef struct {
    /**
     * Load enrollment token from NV storage
     * @param token     Buffer to read into (RTU_ENROLL_TOKEN_LEN bytes)
     * @return 0 on success, -1 on error or not found
     */
    int (*load_token)(char *token);

    /**
     * Save enrollment token to NV storage
     * @param token     Token to save
     * @return 0 on success, -1 on error
     */
    int (*save_token)(const char *token);

    /**
     * Clear enrollment token from NV storage
     * @return 0 on success, -1 on error
     */
    int (*clear_token)(void);
} rtu_registration_nv_ops_t;

/**
 * Register NV storage backend for enrollment persistence
 *
 * @param ops   NV operations (must remain valid)
 * @return RESULT_OK on success
 */
result_t rtu_registration_set_nv_backend(const rtu_registration_nv_ops_t *ops);

#endif /* RTU_REGISTRATION_H */
