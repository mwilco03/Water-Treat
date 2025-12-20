/**
 * @file profinet_callbacks.c
 * @brief PROFINET p-net stack callback implementations
 */

#include "profinet_callbacks.h"
#include "profinet_manager.h"
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
 * State and Connection Callbacks
 * ========================================================================== */

int profinet_state_callback(pnet_t *net, void *arg,
                            uint32_t arep, pnet_event_values_t event) {
    UNUSED(net); UNUSED(arg); UNUSED(arep);
    
    const char *event_str;
    switch (event) {
        case PNET_EVENT_STARTUP: event_str = "STARTUP"; break;
        case PNET_EVENT_PRMEND: event_str = "PRMEND"; break;
        case PNET_EVENT_APPLRDY: event_str = "APPLRDY"; break;
        case PNET_EVENT_ABORT: event_str = "ABORT"; break;
        default: event_str = "UNKNOWN"; break;
    }
    
    LOG_INFO("PROFINET state: %s (arep=%u)", event_str, arep);
    
    if (event == PNET_EVENT_APPLRDY) {
        profinet_manager_set_connected(true);
    } else if (event == PNET_EVENT_ABORT) {
        profinet_manager_set_connected(false);
    }
    
    return 0;
}

int profinet_connect_callback(pnet_t *net, void *arg,
                              uint32_t arep, pnet_result_t *result) {
    UNUSED(net); UNUSED(arg); UNUSED(result);
    
    LOG_INFO("PROFINET connect request (arep=%u)", arep);
    return 0;
}

int profinet_release_callback(pnet_t *net, void *arg,
                              uint32_t arep, pnet_result_t *result) {
    UNUSED(net); UNUSED(arg); UNUSED(result);
    
    LOG_INFO("PROFINET release (arep=%u)", arep);
    profinet_manager_set_connected(false);
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
    
    // Handle parameterization data
    if (idx >= 0x0000 && idx <= 0x7FFF) {
        // Record data write
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
                alarm_arg->slot, alarm_arg->subslot, 
                alarm_arg->alarm_type, data_len);
    
    return 0;
}

int profinet_alarm_cnf_callback(pnet_t *net, void *arg,
                                uint32_t arep, const pnet_pnio_status_t *status) {
    UNUSED(net); UNUSED(arg); UNUSED(arep);
    
    LOG_DEBUG("PROFINET alarm confirmation: error_code=%u", status->error_code);
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
    UNUSED(slot); UNUSED(subslot); UNUSED(idx); UNUSED(seq);
    UNUSED(len); UNUSED(data); UNUSED(result);
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
