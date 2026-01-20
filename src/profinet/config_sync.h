/**
 * @file config_sync.h
 * @brief Configuration sync from SCADA controller via PROFINET
 *
 * Handles configuration packets sent by controller to RTU:
 * - 0xF841: Device configuration
 * - 0xF842: Sensor configuration
 * - 0xF843: Actuator configuration
 *
 * Packet formats defined by controller team (2026-01-20).
 */

#ifndef CONFIG_SYNC_H
#define CONFIG_SYNC_H

#include "common.h"
#include <stdint.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CONFIG_SYNC_VERSION         1
#define CONFIG_SYNC_MAX_SENSORS     8
#define CONFIG_SYNC_MAX_ACTUATORS   7   /* Slots 9-15 */
#define CONFIG_SYNC_NAME_LEN        16
#define CONFIG_SYNC_UNIT_LEN        8
#define CONFIG_SYNC_STATION_LEN     32

/* ============================================================================
 * Device Configuration (0xF841)
 * ============================================================================ */

/**
 * Device configuration packet from controller
 * Total size: 1+1+2+4+32+2+2+1+4 = 49 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t  version;                           /**< Protocol version (1) */
    uint8_t  flags;                             /**< Configuration flags */
    uint16_t crc16;                             /**< CRC16-CCITT of payload */
    uint32_t timestamp;                         /**< Config timestamp (unix) */
    char     station_name[CONFIG_SYNC_STATION_LEN]; /**< Station name */
    uint16_t sensor_count;                      /**< Expected sensor count */
    uint16_t actuator_count;                    /**< Expected actuator count */
    uint8_t  authority_mode;                    /**< Authority: 0=autonomous, 1=supervised */
    uint32_t watchdog_ms;                       /**< Watchdog timeout in ms */
} config_sync_device_packet_t;

/**
 * Device configuration flags
 */
typedef enum {
    CONFIG_FLAG_OVERRIDE_LOCAL  = 0x01,  /**< Override local config */
    CONFIG_FLAG_PERSIST         = 0x02,  /**< Persist to NV storage */
    CONFIG_FLAG_REBOOT_REQUIRED = 0x04,  /**< Reboot after applying */
} config_sync_device_flags_t;

/**
 * Authority modes
 */
typedef enum {
    AUTHORITY_AUTONOMOUS  = 0,  /**< RTU operates independently */
    AUTHORITY_SUPERVISED  = 1,  /**< RTU follows controller commands */
} authority_mode_t;

/* ============================================================================
 * Sensor Configuration (0xF842)
 * ============================================================================ */

/**
 * Single sensor configuration entry
 * Size: 1+1+16+8+4+4+4+4 = 42 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t slot;                              /**< PROFINET slot (1-8) */
    uint8_t type;                              /**< Sensor type enum */
    char    name[CONFIG_SYNC_NAME_LEN];        /**< Display name */
    char    unit[CONFIG_SYNC_UNIT_LEN];        /**< Engineering unit */
    float   scale_min;                         /**< Minimum scaled value */
    float   scale_max;                         /**< Maximum scaled value */
    float   alarm_low;                         /**< Low alarm threshold */
    float   alarm_high;                        /**< High alarm threshold */
} config_sync_sensor_entry_t;

/**
 * Sensor configuration packet header
 * Size: 1+1+2 + entries * 42 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t version;                           /**< Protocol version (1) */
    uint8_t count;                             /**< Number of entries */
    uint16_t crc16;                            /**< CRC16-CCITT of payload */
    /* Followed by count * config_sync_sensor_entry_t */
} config_sync_sensor_packet_t;

/* ============================================================================
 * Actuator Configuration (0xF843)
 * ============================================================================ */

/**
 * Single actuator configuration entry
 * Size: 1+1+16+1+1+2 = 22 bytes (per controller spec)
 */
typedef struct __attribute__((packed)) {
    uint8_t  slot;                             /**< PROFINET slot (9-15) */
    uint8_t  type;                             /**< Actuator type enum */
    char     name[CONFIG_SYNC_NAME_LEN];       /**< Display name */
    uint8_t  default_state;                    /**< Default state on startup */
    uint8_t  reserved;                         /**< Reserved for alignment */
    uint16_t interlock_mask;                   /**< Interlock bitmask */
} config_sync_actuator_entry_t;

/**
 * Actuator configuration packet header
 * Size: 1+1+2 + entries * 21 bytes
 */
typedef struct __attribute__((packed)) {
    uint8_t version;                           /**< Protocol version (1) */
    uint8_t count;                             /**< Number of entries */
    uint16_t crc16;                            /**< CRC16-CCITT of payload */
    /* Followed by count * config_sync_actuator_entry_t */
} config_sync_actuator_packet_t;

/* ============================================================================
 * Status
 * ============================================================================ */

typedef struct {
    uint32_t device_config_count;     /**< Device configs received */
    uint32_t sensor_config_count;     /**< Sensor configs received */
    uint32_t actuator_config_count;   /**< Actuator configs received */
    uint32_t last_device_timestamp;   /**< Last device config timestamp */
    uint8_t  current_authority;       /**< Current authority mode */
    uint8_t  sensors_configured;      /**< Number of sensors configured */
    uint8_t  actuators_configured;    /**< Number of actuators configured */
} config_sync_status_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Initialize config sync subsystem
 */
result_t config_sync_init(void);

/**
 * Shutdown config sync
 */
void config_sync_shutdown(void);

/**
 * Process device configuration packet (0xF841)
 */
result_t config_sync_process_device(const uint8_t *data, uint16_t length);

/**
 * Process sensor configuration packet (0xF842)
 */
result_t config_sync_process_sensors(const uint8_t *data, uint16_t length);

/**
 * Process actuator configuration packet (0xF843)
 */
result_t config_sync_process_actuators(const uint8_t *data, uint16_t length);

/**
 * Get current config sync status
 */
result_t config_sync_get_status(config_sync_status_t *status);

/**
 * Get current authority mode
 */
authority_mode_t config_sync_get_authority(void);

#endif /* CONFIG_SYNC_H */
