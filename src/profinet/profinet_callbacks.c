/**
 * @file profinet_callbacks.c
 * @brief PROFINET p-net stack callback implementations
 *
 * Handles all PROFINET stack callbacks including:
 * - Connection management
 * - Module/submodule configuration
 * - Cyclic data exchange
 * - Acyclic record read/write (including user sync)
 * - Alarms and diagnostics
 */

#include "profinet_callbacks.h"
#include "profinet_manager.h"
#include "rtu_registration.h"
#include "config_sync.h"
#include "auth/user_sync.h"
#include "utils/logger.h"
#include <string.h>
#include <signal.h>
#include <unistd.h>

#ifdef LED_SUPPORT
#include "hal/led_status.h"
extern led_status_manager_t g_led_mgr;
#endif

#ifdef HAVE_PNET
#include <pnet_api.h>

/* I&M0 (Identification & Maintenance) data - mandatory for PROFINET compliance */
#pragma pack(push, 1)
typedef struct {
    uint16_t block_header_type;     // 0x0020 for I&M0
    uint16_t block_header_length;   // 54
    uint8_t  block_header_version;  // 1.0
    uint8_t  block_header_reserved;
    uint16_t vendor_id;             // 0x0493 (matches GSD)
    char     order_id[20];          // Order number (ASCII)
    char     serial_number[16];     // Serial number (ASCII)
    uint16_t hardware_revision;     // HW revision
    struct {
        uint8_t prefix;             // 'V' for release
        uint8_t functional;         // Major version
        uint8_t bugfix;             // Minor version
        uint8_t internal;           // Patch
    } software_revision;
    uint16_t revision_counter;      // Parameter changes counter
    uint16_t profile_id;            // Profile identifier
    uint16_t profile_specific_type; // Profile specific type
    uint16_t im_version;            // 0x0101 = I&M v1.1
    uint16_t im_supported;          // Bit field: I&M0-4 supported
} im0_data_t;
#pragma pack(pop)

static im0_data_t g_im0_data = {
    .block_header_type = 0x0020,
    .block_header_length = 54,
    .block_header_version = 0x01,
    .block_header_reserved = 0,
    .vendor_id = 0x0493,            // Water Treatment Systems vendor ID
    .order_id = "WaterTreat-RTU   ",// 20 chars padded with spaces
    .serial_number = "RTU-000000001   ",// 16 chars
    .hardware_revision = 0x0001,
    .software_revision = {
        .prefix = 'V',
        .functional = 1,            // Major: 1
        .bugfix = 0,                // Minor: 0
        .internal = 0               // Patch: 0
    },
    .revision_counter = 0,
    .profile_id = 0,                // No profile
    .profile_specific_type = 0,
    .im_version = 0x0101,           // I&M v1.1
    .im_supported = 0x001F          // I&M0-4 supported (bits 0-4)
};

/* ============================================================================
 * PNIO Error Code Helpers
 * ========================================================================== */

/**
 * @brief Decode PNIO error codes for human-readable logging
 *
 * PROFINET uses structured error codes in connect/release responses:
 *   - error_code:   Category (0xCF = RTA, 0xDE = PNIO)
 *   - error_code_1: Specific error within category
 *   - error_code_2: Additional detail
 *
 * Common status combinations for AR (Application Relationship) errors:
 *   status1=0x00000001, status2=0x00000003:
 *     "AR already exists" - stale AR from previous connection
 *     Fix: Clear p-net NV files or reboot RTU
 *
 *   status1=0x00000001, status2=0x00000004:
 *     "Session key mismatch" - controller/RTU out of sync
 *     Fix: Clear p-net NV files
 *
 *   status1=0x00000001, status2=0x00000001:
 *     "Configuration mismatch" - GSDML/module config doesn't match
 *     Fix: Verify GSDML matches between controller and RTU
 */
static const char* decode_pnio_ar_error(uint32_t status2) {
    switch (status2) {
        case 0x00000001: return "Configuration mismatch (GSDML/module)";
        case 0x00000002: return "AR type not supported";
        case 0x00000003: return "AR already exists (stale connection state)";
        case 0x00000004: return "Session key mismatch";
        case 0x00000005: return "No AR resource available";
        case 0x00000006: return "AR UUID already in use";
        case 0x00000007: return "Parameter error";
        case 0x00000008: return "Alarm type not supported";
        case 0x00000009: return "RPC interface not supported";
        default: return "Unknown AR error";
    }
}

/**
 * @brief Log PNIO error details for debugging connection issues
 */
static void log_pnio_result(const char *context, const pnet_result_t *result) {
    if (!result) return;

    uint8_t err_cls = result->pnio_status.error_code;
    uint8_t err_code = result->pnio_status.error_code_1;
    uint16_t detail = result->pnio_status.error_code_2;

    /* Construct status values as seen in pcap */
    uint32_t status1 = ((uint32_t)err_cls << 24) | ((uint32_t)err_code << 16);
    uint32_t status2 = detail;

    if (err_cls == 0 && err_code == 0 && detail == 0) {
        /* Success - no error */
        return;
    }

    LOG_WARNING("PROFINET %s PNIO error: status1=0x%08X, status2=0x%08X",
                context, status1, status2);

    /* Decode error class */
    const char *class_str;
    switch (err_cls) {
        case 0xCF: class_str = "RTA error"; break;
        case 0xDE: class_str = "PNIO-specific"; break;
        case 0xDF: class_str = "IOD block error"; break;
        default: class_str = "Unknown"; break;
    }

    LOG_WARNING("  Error class: 0x%02X (%s), code: 0x%02X, detail: 0x%04X",
                err_cls, class_str, err_code, detail);

    /* Special handling for AR block errors (status1 high byte = 0x01) */
    if (status1 == 0x00000001 || err_cls == 0x01) {
        LOG_WARNING("  AR error: %s", decode_pnio_ar_error(status2));
        LOG_WARNING("  Recommended fix: Run 'systemctl restart water-treat' or clear p-net NV files");
    }
}

/* ============================================================================
 * State and Connection Callbacks
 * ========================================================================== */

int profinet_state_callback(pnet_t *net, void *arg,
                            uint32_t arep, pnet_event_values_t event) {
    UNUSED(arg);

    const char *event_str;
    switch (event) {
        case PNET_EVENT_STARTUP: event_str = "STARTUP"; break;
        case PNET_EVENT_PRMEND: event_str = "PRMEND"; break;
        case PNET_EVENT_APPLRDY: event_str = "APPLRDY"; break;
        case PNET_EVENT_ABORT: event_str = "ABORT"; break;
        case PNET_EVENT_DATA: event_str = "DATA"; break;
        default: event_str = "UNKNOWN"; break;
    }

    LOG_INFO("PROFINET state: %s (arep=%u)", event_str, arep);

    if (event == PNET_EVENT_PRMEND) {
        /*
         * Parameterization complete - signal application ready to controller.
         * This is REQUIRED for the connection handshake to complete.
         * Without this call, the controller never receives a response
         * and the connection times out.
         *
         * CRITICAL: Before calling pnet_application_ready(), we MUST initialize
         * all input subslots with data and IOPS. The p-net library checks that
         * all plugged INPUT subslots have valid data set via pnet_input_set_data_and_iops()
         * before it will send the CControl (APPL_RDY) response to the controller.
         *
         * If pnet_application_ready() returns -1, it means:
         *   - Not all input data is set, OR
         *   - Not all IOPS values are set, OR
         *   - Internal p-net error
         */
        int inputs_initialized = profinet_manager_init_all_inputs();
        LOG_DEBUG("Initialized %d input subslots before application_ready", inputs_initialized);

        int ret = pnet_application_ready(net, arep);
        if (ret != 0) {
            LOG_ERROR("pnet_application_ready() failed: %d", ret);
            LOG_ERROR("This usually means input data/IOPS not set for all plugged input subslots.");
            LOG_ERROR("Controller will timeout waiting for CControl response.");
            /*
             * Note: We don't abort here - p-net may still recover in some cases.
             * The controller will timeout and may retry the connection.
             */
        } else {
            LOG_INFO("Application ready signaled to controller (arep=%u)", arep);
        }
    } else if (event == PNET_EVENT_APPLRDY) {
        /*
         * Controller acknowledged our APPL_RDY - connection is now established.
         * Cyclic data exchange can begin.
         */
        LOG_INFO("Controller acknowledged APPL_RDY - connection established");
        profinet_manager_set_connected(true, arep);
    } else if (event == PNET_EVENT_DATA) {
        /*
         * Cyclic data exchange is now active.
         * This event indicates the controller is sending/receiving cyclic data.
         */
        LOG_INFO("Cyclic data exchange active (arep=%u)", arep);
    } else if (event == PNET_EVENT_ABORT) {
        /*
         * Connection aborted - either by controller or due to error.
         * Reset connection state and wait for new connection.
         *
         * Common causes of ABORT:
         * 1. AR already exists (PNIO status1=0x1, status2=0x3) - stale AR state
         *    Fix: Clear p-net NV files with bootstrap.sh or restart service
         * 2. Controller timeout waiting for APPL_RDY
         * 3. Configuration mismatch between controller and RTU
         * 4. Network disconnection
         */
        LOG_WARNING("Connection aborted (arep=%u) - AR state cleared", arep);
        LOG_WARNING("If this persists, run: sudo systemctl restart water-treat");
        LOG_WARNING("Or clear stale state: sudo rm -f /var/lib/water-treat/pnet/pf_*");
        profinet_manager_set_connected(false, 0);
    }

    return 0;
}

int profinet_connect_callback(pnet_t *net, void *arg,
                              uint32_t arep, pnet_result_t *result) {
    UNUSED(net); UNUSED(arg);

    LOG_INFO("PROFINET connect request (arep=%u)", arep);

    /* Log any PNIO errors from the connect response */
    if (result) {
        log_pnio_result("connect response", result);
    }

    return 0;
}

int profinet_release_callback(pnet_t *net, void *arg,
                              uint32_t arep, pnet_result_t *result) {
    UNUSED(net); UNUSED(arg);

    LOG_INFO("PROFINET release (arep=%u)", arep);

    /* Log any PNIO errors explaining why connection was released */
    if (result) {
        log_pnio_result("release", result);
    }

    profinet_manager_set_connected(false, 0);
    return 0;
}

int profinet_dcontrol_callback(pnet_t *net, void *arg,
                               uint32_t arep, pnet_control_command_t command,
                               pnet_result_t *result) {
    UNUSED(net); UNUSED(arg); UNUSED(result);
    
    const char *cmd_str;
    switch (command) {
        case PNET_CONTROL_COMMAND_PRM_BEGIN: cmd_str = "PRM_BEGIN"; break;
        case PNET_CONTROL_COMMAND_PRM_END: cmd_str = "PRM_END"; break;
        case PNET_CONTROL_COMMAND_APP_RDY: cmd_str = "APP_RDY"; break;
        case PNET_CONTROL_COMMAND_RELEASE: cmd_str = "RELEASE"; break;
        default: cmd_str = "UNKNOWN"; break;
    }
    
    LOG_DEBUG("PROFINET DControl: %s (arep=%u)", cmd_str, arep);
    return 0;
}

int profinet_ccontrol_callback(pnet_t *net, void *arg,
                               uint32_t arep, pnet_result_t *result) {
    UNUSED(net); UNUSED(arg); UNUSED(result);
    
    LOG_DEBUG("PROFINET CControl confirmation (arep=%u)", arep);
    return 0;
}

/* ============================================================================
 * Data Read/Write Callbacks
 * ========================================================================== */

int profinet_read_callback(pnet_t *net, void *arg,
                           uint32_t arep, uint32_t api, uint16_t slot, uint16_t subslot,
                           uint16_t idx, uint16_t sequence_number,
                           uint8_t **data, uint16_t *length,
                           pnet_result_t *result) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(api);
    UNUSED(sequence_number); UNUSED(result);
    
    LOG_DEBUG("PROFINET read: slot=%u.%u, idx=0x%04X", slot, subslot, idx);
    
    // Handle standard indices
    switch (idx) {
        case 0x8000:  // Identification & Maintenance 0 (mandatory)
            *data = (uint8_t *)&g_im0_data;
            *length = sizeof(im0_data_t);
            LOG_DEBUG("I&M0 read: providing %u bytes", *length);
            break;

        case 0x8001:  // Identification & Maintenance 1 (optional - function tag)
        case 0x8002:  // Identification & Maintenance 2 (optional - date)
        case 0x8003:  // Identification & Maintenance 3 (optional - descriptor)
        case 0x8004:  // Identification & Maintenance 4 (optional - signature)
            // Optional I&M records not implemented
            LOG_DEBUG("I&M%d read: not implemented", idx - 0x8000);
            *data = NULL;
            *length = 0;
            break;

        default:
            // Application-specific read
            *data = NULL;
            *length = 0;
            break;
    }
    
    return 0;
}

int profinet_write_callback(pnet_t *net, void *arg,
                            uint32_t arep, uint32_t api, uint16_t slot, uint16_t subslot,
                            uint16_t idx, uint16_t sequence_number,
                            uint16_t write_length, const uint8_t *data,
                            pnet_result_t *result) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(api);
    UNUSED(sequence_number); UNUSED(result);

    LOG_DEBUG("PROFINET write: slot=%u.%u, idx=0x%04X, len=%u",
              slot, subslot, idx, write_length);

    /*
     * Handle vendor-specific PROFINET record writes (0xF840-0xF845)
     *
     * Index allocation:
     * - 0xF840: User sync (credentials from controller)
     * - 0xF841: Device configuration
     * - 0xF842: Sensor configuration
     * - 0xF843: Actuator configuration
     * - 0xF845: Enrollment/binding
     */
    switch (idx) {
        case USER_SYNC_PROFINET_INDEX: /* 0xF840 */
            /*
             * User sync packets from controller
             *
             * The controller sends user credentials via PROFINET acyclic write.
             * This allows centralized user management with credentials synced to
             * RTU for local authentication when controller is offline.
             *
             * Security: User sync uses DJB2 hashed passwords, not plaintext.
             * The hash format is "DJB2:<salt_hash>:<password_hash>".
             */
            LOG_INFO("Received user sync packet: %u bytes", write_length);

            {
                result_t r = user_sync_process_packet(data, write_length);
                if (r != RESULT_OK) {
                    LOG_ERROR("User sync packet processing failed: %d", r);
                    if (result) {
                        result->pnio_status.error_code = 0xCF;
                        result->pnio_status.error_code_1 = 0x81;
                    }
                    return -1;
                }

                user_sync_status_t status;
                user_sync_get_status(&status);
                LOG_INFO("User sync complete: %u users stored, %u total syncs",
                         status.users_stored, status.sync_count);
            }
            return 0;

        case RTU_ENROLL_PROFINET_INDEX: /* 0xF845 */
            /*
             * Enrollment/binding packets from controller
             *
             * Used to confirm RTU registration and store enrollment token.
             * Supports bind, unbind, rebind, and status query operations.
             */
            LOG_INFO("Received enrollment packet: %u bytes", write_length);

            {
                result_t r = rtu_registration_process_enrollment(data, write_length);
                if (r != RESULT_OK) {
                    LOG_ERROR("Enrollment packet processing failed: %d", r);
                    if (result) {
                        result->pnio_status.error_code = 0xCF;
                        result->pnio_status.error_code_1 = 0x81;
                    }
                    return -1;
                }
            }
            return 0;

        case RTU_CONFIG_PROFINET_INDEX: /* 0xF841 */
            LOG_INFO("Received device config packet: %u bytes", write_length);
            {
                result_t r = config_sync_process_device(data, write_length);
                if (r != RESULT_OK) {
                    LOG_ERROR("Device config processing failed: %d", r);
                    if (result) {
                        result->pnio_status.error_code = 0xCF;
                        result->pnio_status.error_code_1 = 0x81;
                    }
                    return -1;
                }
            }
            return 0;

        case RTU_SENSOR_CONFIG_PROFINET_INDEX: /* 0xF842 */
            LOG_INFO("Received sensor config packet: %u bytes", write_length);
            {
                result_t r = config_sync_process_sensors(data, write_length);
                if (r != RESULT_OK) {
                    LOG_ERROR("Sensor config processing failed: %d", r);
                    if (result) {
                        result->pnio_status.error_code = 0xCF;
                        result->pnio_status.error_code_1 = 0x81;
                    }
                    return -1;
                }
            }
            return 0;

        case RTU_ACTUATOR_CONFIG_PROFINET_INDEX: /* 0xF843 */
            LOG_INFO("Received actuator config packet: %u bytes", write_length);
            {
                result_t r = config_sync_process_actuators(data, write_length);
                if (r != RESULT_OK) {
                    LOG_ERROR("Actuator config processing failed: %d", r);
                    if (result) {
                        result->pnio_status.error_code = 0xCF;
                        result->pnio_status.error_code_1 = 0x81;
                    }
                    return -1;
                }
            }
            return 0;

        default:
            break;
    }

    /* Handle standard parameterization data */
    if (idx <= 0x7FFF) {
        LOG_INFO("Parameter write slot %u.%u idx 0x%04X: %u bytes",
                 slot, subslot, idx, write_length);
    }

    return 0;
}

/* ============================================================================
 * Module/Submodule Callbacks
 * ========================================================================== */

int profinet_exp_module_callback(pnet_t *net, void *arg,
                                 uint32_t api, uint16_t slot,
                                 uint32_t module_ident) {
    UNUSED(net); UNUSED(arg); UNUSED(api);
    
    LOG_INFO("PROFINET expected module: slot=%u, ident=0x%08X", slot, module_ident);
    
    // Accept the module - actual plugging is done in profinet_manager_start
    return 0;
}

int profinet_exp_submodule_callback(pnet_t *net, void *arg,
                                    uint32_t api, uint16_t slot, uint16_t subslot,
                                    uint32_t module_ident, uint32_t submodule_ident,
                                    const pnet_data_cfg_t *exp_data_cfg) {
    UNUSED(net); UNUSED(arg); UNUSED(api);
    UNUSED(module_ident); UNUSED(exp_data_cfg);
    
    LOG_INFO("PROFINET expected submodule: slot=%u.%u, ident=0x%08X", 
             slot, subslot, submodule_ident);
    
    return 0;
}

/* ============================================================================
 * Data Status Callback
 * ========================================================================== */

int profinet_new_data_status_callback(pnet_t *net, void *arg,
                                      uint32_t arep, uint32_t crep,
                                      uint8_t changes, uint8_t data_status) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(crep);
    
    bool run = (data_status & 0x04) != 0;
    bool valid = (data_status & 0x01) != 0;
    
    if (changes) {
        LOG_DEBUG("PROFINET data status: run=%d, valid=%d, status=0x%02X", 
                  run, valid, data_status);
    }
    
    return 0;
}

/* ============================================================================
 * Alarm Callbacks
 * ========================================================================== */

int profinet_alarm_ind_callback(pnet_t *net, void *arg,
                                uint32_t arep, const pnet_alarm_argument_t *alarm_arg,
                                uint16_t data_len, uint16_t data_usi,
                                const uint8_t *data) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(data_usi); UNUSED(data);
    
    LOG_WARNING("PROFINET alarm indication: slot=%u.%u, type=0x%04X, len=%u",
                alarm_arg->slot_nbr, alarm_arg->subslot_nbr,
                alarm_arg->alarm_type, data_len);
    
    return 0;
}

int profinet_alarm_cnf_callback(pnet_t *net, void *arg,
                                uint32_t arep, const pnet_pnio_status_t *status) {
    UNUSED(net); UNUSED(arg); UNUSED(arep);

    if (status && (status->error_code != 0 || status->error_code_1 != 0)) {
        LOG_WARNING("PROFINET alarm confirmation error: class=0x%02X, code=0x%02X, detail=0x%04X",
                    status->error_code, status->error_code_1, status->error_code_2);
    } else {
        LOG_DEBUG("PROFINET alarm confirmation: OK");
    }
    return 0;
}

int profinet_alarm_ack_cnf_callback(pnet_t *net, void *arg,
                                    uint32_t arep, int res) {
    UNUSED(net); UNUSED(arg); UNUSED(arep);
    
    LOG_DEBUG("PROFINET alarm ack confirmation: res=%d", res);
    return 0;
}

/* ============================================================================
 * System Callbacks
 * ========================================================================== */

int profinet_reset_callback(pnet_t *net, void *arg,
                            bool should_reset_application,
                            uint16_t reset_mode) {
    UNUSED(net); UNUSED(arg);

    LOG_WARNING("PROFINET reset request: app_reset=%d, mode=%u",
                should_reset_application, reset_mode);

    if (should_reset_application) {
        switch (reset_mode) {
            case 0:
                /* Reset mode 0: Reload configuration (soft reset) */
                LOG_INFO("Triggering configuration reload (SIGHUP)");
                kill(getpid(), SIGHUP);
                break;

            case 1:
                /* Reset mode 1: Factory reset - restart application */
                LOG_WARNING("Factory reset requested - restarting application");
                /* SIGTERM triggers clean shutdown; systemd will restart the service */
                kill(getpid(), SIGTERM);
                break;

            case 2:
                /* Reset mode 2: Reset to factory with backup (not implemented) */
                LOG_WARNING("Factory reset with backup not implemented, performing soft reset");
                kill(getpid(), SIGHUP);
                break;

            default:
                LOG_WARNING("Unknown reset mode %u, performing soft reset", reset_mode);
                kill(getpid(), SIGHUP);
                break;
        }
    }

    return 0;
}

int profinet_signal_led_callback(pnet_t *net, void *arg, bool led_state) {
    UNUSED(net); UNUSED(arg);

    LOG_INFO("PROFINET signal LED: %s (device identification)", led_state ? "ON" : "OFF");

#ifdef LED_SUPPORT
    /* Drive system LED for device identification (blink when signaled by controller) */
    if (g_led_mgr.initialized) {
        if (led_state) {
            /* Flash system LED rapidly to identify this device */
            led_set_custom(&g_led_mgr, LED_FUNC_SYSTEM, LED_COLOR_CYAN, LED_ANIM_BLINK_FAST);
        } else {
            /* Restore normal system status */
            led_set_system_status(&g_led_mgr, LED_STATUS_OK);
        }
    }
#endif

    return 0;
}

#else /* !HAVE_PNET */

/* Stub implementations when p-net is not available */

int profinet_state_callback(void *net, void *arg, uint32_t arep, int event) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(event);
    return 0;
}

int profinet_connect_callback(void *net, void *arg, uint32_t arep, void *result) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(result);
    return 0;
}

int profinet_release_callback(void *net, void *arg, uint32_t arep, void *result) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(result);
    return 0;
}

int profinet_dcontrol_callback(void *net, void *arg, uint32_t arep, int command, void *result) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(command); UNUSED(result);
    return 0;
}

int profinet_ccontrol_callback(void *net, void *arg, uint32_t arep, void *result) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(result);
    return 0;
}

int profinet_read_callback(void *net, void *arg, uint32_t arep, uint32_t api,
                           uint16_t slot, uint16_t subslot, uint16_t idx,
                           uint16_t seq, uint8_t **data, uint16_t *length, void *result) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(api);
    UNUSED(slot); UNUSED(subslot); UNUSED(idx); UNUSED(seq);
    UNUSED(data); UNUSED(length); UNUSED(result);
    return 0;
}

int profinet_write_callback(void *net, void *arg, uint32_t arep, uint32_t api,
                            uint16_t slot, uint16_t subslot, uint16_t idx,
                            uint16_t seq, uint16_t len, const uint8_t *data, void *result) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(api);
    UNUSED(slot); UNUSED(subslot); UNUSED(seq); UNUSED(result);

    /* Handle user sync even in stub mode (for testing) */
    if (idx == USER_SYNC_PROFINET_INDEX && data && len > 0) {
        LOG_INFO("User sync packet received (stub mode): %u bytes", len);
        user_sync_process_packet(data, len);
    }

    /* Handle enrollment in stub mode (for testing) */
    if (idx == RTU_ENROLL_PROFINET_INDEX && data && len > 0) {
        LOG_INFO("Enrollment packet received (stub mode): %u bytes", len);
        rtu_registration_process_enrollment(data, len);
    }

    return 0;
}

int profinet_exp_module_callback(void *net, void *arg, uint32_t api,
                                 uint16_t slot, uint32_t module_ident) {
    UNUSED(net); UNUSED(arg); UNUSED(api); UNUSED(slot); UNUSED(module_ident);
    return 0;
}

int profinet_exp_submodule_callback(void *net, void *arg, uint32_t api,
                                    uint16_t slot, uint16_t subslot,
                                    uint32_t module_ident, uint32_t submodule_ident,
                                    const void *exp_data_cfg) {
    UNUSED(net); UNUSED(arg); UNUSED(api);
    UNUSED(slot); UNUSED(subslot);
    UNUSED(module_ident); UNUSED(submodule_ident); UNUSED(exp_data_cfg);
    return 0;
}

int profinet_new_data_status_callback(void *net, void *arg, uint32_t arep,
                                      uint32_t crep, uint8_t changes, uint8_t data_status) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(crep);
    UNUSED(changes); UNUSED(data_status);
    return 0;
}

int profinet_alarm_ind_callback(void *net, void *arg, uint32_t arep,
                                const void *alarm_arg, uint16_t data_len,
                                uint16_t data_usi, const uint8_t *data) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(alarm_arg);
    UNUSED(data_len); UNUSED(data_usi); UNUSED(data);
    return 0;
}

int profinet_alarm_cnf_callback(void *net, void *arg, uint32_t arep, const void *status) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(status);
    return 0;
}

int profinet_alarm_ack_cnf_callback(void *net, void *arg, uint32_t arep, int res) {
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(res);
    return 0;
}

int profinet_reset_callback(void *net, void *arg, bool should_reset, uint16_t mode) {
    UNUSED(net); UNUSED(arg); UNUSED(should_reset); UNUSED(mode);
    return 0;
}

int profinet_signal_led_callback(void *net, void *arg, bool led_state) {
    UNUSED(net); UNUSED(arg); UNUSED(led_state);
    return 0;
}

#endif /* HAVE_PNET */
