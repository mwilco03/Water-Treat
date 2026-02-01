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
#include "config/config.h"
#include "utils/logger.h"
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <arpa/inet.h>  /* htons for I&M0 byte ordering */

/* Global config from main.c — needed for factory reset backup paths */
extern config_manager_t g_config_mgr;
extern app_config_t g_app_config;

#ifdef LED_SUPPORT
#include "hal/led_status.h"
extern led_status_manager_t g_led_mgr;
#endif

#ifdef HAVE_PNET
#include <pnet_api.h>
#include "gsdml_modules.h"

/* Static buffer for 0xF844 slot map record read response.
 * Must persist until controller completes the read, so it's static.
 * Max: 2-byte header + 246 slots * 15 bytes each = 3692 bytes */
#define SLOT_MAP_RECORD_MAX_SIZE (2 + 246 * 15)
static uint8_t g_slot_map_buffer[SLOT_MAP_RECORD_MAX_SIZE];
static uint16_t g_slot_map_length = 0;

/* I&M0 (Identification & Maintenance) data - mandatory for PROFINET compliance */
#pragma pack(push, 1)
typedef struct {
    uint16_t block_header_type;     // 0x0020 for I&M0
    uint16_t block_header_length;   // 54
    uint8_t  block_header_version;  // 1.0
    uint8_t  block_header_reserved;
    uint16_t vendor_id;             // 0x0493 (matches GSD)
    char     order_id[20];          // Order number (ASCII, space-padded)
    char     serial_number[16];     // Serial number (ASCII, space-padded)
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

/*
 * I&M0 data — initialized at runtime by profinet_callbacks_init()
 * to ensure multi-byte fields are in network byte order (big-endian).
 *
 * PROFINET record data is transmitted as raw bytes; the stack does NOT
 * byte-swap application callback data.  A static initializer would store
 * uint16_t fields in host byte order, producing wrong values on
 * little-endian systems (ARM, x86).
 */
static im0_data_t g_im0_data;
static bool g_im0_initialized = false;

/* ============================================================================
 * Initialization
 * ========================================================================== */

void profinet_callbacks_init(void) {
    if (g_im0_initialized) return;

    /*
     * Initialize I&M0 with multi-byte fields in network byte order.
     * Text fields are space-padded per PROFINET I&M0 specification
     * (IEC 61158-6-10, 5.2.18.1).
     */
    memset(&g_im0_data, 0, sizeof(g_im0_data));

    /* Space-pad text fields before copying content */
    memset(g_im0_data.order_id, ' ', sizeof(g_im0_data.order_id));
    memset(g_im0_data.serial_number, ' ', sizeof(g_im0_data.serial_number));

    /* Block header — big-endian */
    g_im0_data.block_header_type    = htons(0x0020);
    g_im0_data.block_header_length  = htons(54);
    g_im0_data.block_header_version = 0x01;
    g_im0_data.block_header_reserved = 0x00;

    /* Device identity — big-endian */
    g_im0_data.vendor_id = htons(PN_VENDOR_ID);
    memcpy(g_im0_data.order_id, "WaterTreat-RTU", 14);         /* 14 chars + space-pad */
    memcpy(g_im0_data.serial_number, "RTU-000000001", 13);     /* 13 chars + space-pad */

    /* Revisions */
    g_im0_data.hardware_revision         = htons(0x0001);
    g_im0_data.software_revision.prefix     = 'V';
    g_im0_data.software_revision.functional = 1;   /* Major */
    g_im0_data.software_revision.bugfix     = 0;   /* Minor */
    g_im0_data.software_revision.internal   = 0;   /* Patch */

    /* Profile — big-endian */
    g_im0_data.revision_counter      = htons(0);
    g_im0_data.profile_id            = htons(0);
    g_im0_data.profile_specific_type = htons(0);
    g_im0_data.im_version            = htons(0x0101);  /* I&M v1.1 */
    g_im0_data.im_supported          = htons(0x001F);  /* I&M0-4 */

    g_im0_initialized = true;
    LOG_DEBUG("I&M0 data initialized (%zu bytes, network byte order)",
              sizeof(im0_data_t));
}

/* ============================================================================
 * PNIO Error Recovery
 * ========================================================================== */

/* Forward declaration - implemented in profinet_manager.c */
void profinet_manager_clear_ar_state(void);

/**
 * @brief Check PNIO result and auto-recover from transient errors
 *
 * Recoverable errors (auto-fix and retry):
 *   0x03 - AR already exists (stale state) -> clear AR, ready for retry
 *   0x04 - Session key mismatch -> clear session state, ready for retry
 *
 * Non-recoverable (log only, needs external fix):
 *   0x01 - Configuration mismatch (GSDML problem)
 *
 * @return true if error was recoverable and state was cleared
 */
static bool handle_pnio_error(const pnet_result_t *result) {
    if (!result) return false;

    uint16_t detail = result->pnio_status.error_code_2;

    /* No error */
    if (result->pnio_status.error_code == 0 &&
        result->pnio_status.error_code_1 == 0 && detail == 0) {
        return false;
    }

    switch (detail) {
        case 0x0003:  /* AR already exists - stale connection */
            LOG_INFO("Stale AR detected, clearing state for retry");
            profinet_manager_clear_ar_state();
            return true;

        case 0x0004:  /* Session key mismatch */
            LOG_INFO("Session key mismatch, clearing state for retry");
            profinet_manager_clear_ar_state();
            return true;

        case 0x0001:  /* Configuration mismatch - can't auto-fix */
            LOG_ERROR("GSDML/module configuration mismatch - check controller config");
            return false;

        default:
            LOG_DEBUG("PNIO error 0x%04X", detail);
            return false;
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
         * Parameterization complete - RTU must send ApplicationReady TO controller.
         * Protocol sequence:
         *   1. Controller sends PrmEnd to RTU (we just received it)
         *   2. RTU sends ApplicationReady to Controller (this call)
         *   3. Controller responds with acknowledgment
         *   4. RTU gets PNET_EVENT_APPLRDY
         *
         * CRITICAL: Initialize all inputs before signaling ready.
         */
        int inputs_initialized = profinet_manager_init_all_inputs();
        LOG_DEBUG("Initialized %d input subslots", inputs_initialized);

        int ret = pnet_application_ready(net, arep);
        if (ret != 0) {
            LOG_WARNING("pnet_application_ready() failed: %d - clearing state for retry", ret);
            profinet_manager_clear_ar_state();
        } else {
            LOG_INFO("Sent ApplicationReady to controller (arep=%u)", arep);
        }
    } else if (event == PNET_EVENT_APPLRDY) {
        /* Controller acknowledged our ApplicationReady - connection established */
        LOG_INFO("Connection established (arep=%u)", arep);
        profinet_manager_set_connected(true, arep);
    } else if (event == PNET_EVENT_DATA) {
        /*
         * Cyclic data exchange is now active.
         * This event indicates the controller is sending/receiving cyclic data.
         */
        LOG_INFO("Cyclic data exchange active (arep=%u)", arep);
    } else if (event == PNET_EVENT_ABORT) {
        /* Connection aborted - clear state and be ready for reconnect */
        LOG_INFO("Connection aborted (arep=%u), clearing AR state", arep);
        profinet_manager_set_connected(false, 0);
        profinet_manager_clear_ar_state();
    }

    return 0;
}

/* Forward declaration for state tracking */
void profinet_manager_set_connecting(void);

int profinet_connect_callback(pnet_t *net, void *arg,
                              uint32_t arep, pnet_result_t *result) {
    UNUSED(net); UNUSED(arg);

    LOG_INFO(">>> CALLBACK: connect_callback arep=%u (RPC Connect Request accepted by p-net!)", arep);

    /* Track connection attempt in state machine */
    profinet_manager_set_connecting();

    /* Auto-recover from transient errors - controller will retry */
    if (result) {
        if (result->pnio_status.error_code != 0 ||
            result->pnio_status.error_code_1 != 0 ||
            result->pnio_status.error_code_2 != 0) {
            LOG_WARNING(">>> connect_callback result: err=0x%02X, err1=0x%02X, err2=0x%04X",
                        result->pnio_status.error_code,
                        result->pnio_status.error_code_1,
                        result->pnio_status.error_code_2);
        }
        handle_pnio_error(result);
    }

    return 0;
}

int profinet_release_callback(pnet_t *net, void *arg,
                              uint32_t arep, pnet_result_t *result) {
    UNUSED(net); UNUSED(arg);

    LOG_INFO("PROFINET release (arep=%u)", arep);

    if (result &&
        (result->pnio_status.error_code != 0 ||
         result->pnio_status.error_code_1 != 0 ||
         result->pnio_status.error_code_2 != 0)) {
        LOG_WARNING(">>> release_callback result: err=0x%02X, err1=0x%02X, err2=0x%04X",
                    result->pnio_status.error_code,
                    result->pnio_status.error_code_1,
                    result->pnio_status.error_code_2);
    }

    profinet_manager_set_connected(false, 0);
    return 0;
}

int profinet_dcontrol_callback(pnet_t *net, void *arg,
                               uint32_t arep, pnet_control_command_t command,
                               pnet_result_t *result) {
    UNUSED(net); UNUSED(arg); UNUSED(result);

    switch (command) {
        case PNET_CONTROL_COMMAND_PRM_BEGIN:
            LOG_DEBUG("DControl: PRM_BEGIN (arep=%u)", arep);
            break;
        case PNET_CONTROL_COMMAND_PRM_END:
            LOG_DEBUG("DControl: PRM_END (arep=%u)", arep);
            break;
        case PNET_CONTROL_COMMAND_APP_RDY:
            /*
             * Controller sent APP_RDY to us - this is BACKWARDS.
             * Per PROFINET spec: RTU sends ApplicationReady TO controller.
             * Controller should be LISTENING, not sending.
             * This indicates controller bug.
             */
            LOG_WARNING("Controller sent APP_RDY to RTU - protocol violation!");
            LOG_WARNING("RTU sends ApplicationReady TO controller, not vice versa");
            break;
        case PNET_CONTROL_COMMAND_RELEASE:
            LOG_DEBUG("DControl: RELEASE (arep=%u)", arep);
            break;
        default:
            LOG_DEBUG("DControl: UNKNOWN (arep=%u, cmd=%d)", arep, command);
            break;
    }
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
    UNUSED(sequence_number);

    LOG_DEBUG("PROFINET read: slot=%u.%u, idx=0x%04X", slot, subslot, idx);

    switch (idx) {
        case 0x8000:  /* Identification & Maintenance 0 (mandatory) */
            *data = (uint8_t *)&g_im0_data;
            *length = sizeof(im0_data_t);
            LOG_DEBUG("I&M0 read: providing %u bytes", *length);
            return 0;

        case 0x8001:  /* Identification & Maintenance 1 (optional - function tag) */
        case 0x8002:  /* Identification & Maintenance 2 (optional - date) */
        case 0x8003:  /* Identification & Maintenance 3 (optional - descriptor) */
        case 0x8004:  /* Identification & Maintenance 4 (optional - signature) */
            LOG_DEBUG("I&M%d read: optional, not supported", idx - 0x8000);
            *data = NULL;
            *length = 0;
            return 0;

        case 0xF844: {
            /*
             * Slot map record read - PROFINET fallback for slot discovery.
             * Step 5 in the controller's discovery chain (used when HTTP
             * endpoints are unreachable on isolated L2 networks).
             *
             * Wire format: 2-byte count + 15 bytes per slot (all BE).
             * See profinet_manager_build_slot_map() for format details.
             */
            int ret = profinet_manager_build_slot_map(
                g_slot_map_buffer, sizeof(g_slot_map_buffer));
            if (ret > 0) {
                g_slot_map_length = (uint16_t)ret;
                *data = g_slot_map_buffer;
                *length = g_slot_map_length;
                LOG_INFO("Slot map read (0xF844): providing %u bytes", *length);
            } else {
                g_slot_map_length = 0;
                *data = NULL;
                *length = 0;
                LOG_INFO("Slot map read (0xF844): no application modules");
            }
            return 0;
        }

        default:
            /* Standard parameter indices (0x0000-0x7FFF) - let p-net handle */
            if (idx <= 0x7FFF) {
                LOG_DEBUG("Standard read index 0x%04X, deferring to p-net", idx);
                *data = NULL;
                *length = 0;
                return 0;
            }
            /* Unknown vendor-specific index - reject */
            LOG_WARNING("Unknown read index 0x%04X on slot %u.%u, rejecting",
                        idx, slot, subslot);
            if (result) {
                result->pnio_status.error_code = 0xDE;   /* IODReadRes */
                result->pnio_status.error_code_1 = 0x80; /* Application read error */
            }
            return -1;
    }
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
            /* Standard parameterization data (0x0000-0x7FFF) is accepted */
            if (idx <= 0x7FFF) {
                LOG_INFO("Parameter write slot %u.%u idx 0x%04X: %u bytes",
                         slot, subslot, idx, write_length);
                return 0;
            }
            /* Unknown vendor-specific index — reject with error */
            LOG_WARNING("Unknown write index 0x%04X on slot %u.%u, rejecting",
                        idx, slot, subslot);
            if (result) {
                result->pnio_status.error_code = 0xDE;   /* IODWriteRes */
                result->pnio_status.error_code_1 = 0x80; /* Application write error */
            }
            return -1;
    }
}

/* ============================================================================
 * Module/Submodule Callbacks
 * ========================================================================== */

int profinet_exp_module_callback(pnet_t *net, void *arg,
                                 uint32_t api, uint16_t slot,
                                 uint32_t module_ident) {
    UNUSED(net); UNUSED(arg); UNUSED(api);

    LOG_INFO(">>> CALLBACK: exp_module slot=%u, ident=0x%08X (p-net reached app layer!)",
             slot, module_ident);

    // Accept the module - actual plugging is done in profinet_manager_start
    return 0;
}

int profinet_exp_submodule_callback(pnet_t *net, void *arg,
                                    uint32_t api, uint16_t slot, uint16_t subslot,
                                    uint32_t module_ident, uint32_t submodule_ident,
                                    const pnet_data_cfg_t *exp_data_cfg) {
    UNUSED(net); UNUSED(arg); UNUSED(api);
    UNUSED(module_ident); UNUSED(exp_data_cfg);

    LOG_INFO(">>> CALLBACK: exp_submodule slot=%u.%u, submod=0x%08X (module processing OK)",
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
    UNUSED(net); UNUSED(arg); UNUSED(arep); UNUSED(status);
    LOG_DEBUG("PROFINET alarm confirmed");
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

#define BACKUP_DIR "/var/backup/water-treat"

/**
 * @brief Copy a file to a destination path.
 * @return true on success, false on failure (logged).
 */
static bool backup_copy_file(const char *src, const char *dst) {
    FILE *src_fp = fopen(src, "rb");
    if (!src_fp) {
        LOG_ERROR("Backup: cannot open source %s: %s", src, strerror(errno));
        return false;
    }

    FILE *dst_fp = fopen(dst, "wb");
    if (!dst_fp) {
        LOG_ERROR("Backup: cannot create %s: %s", dst, strerror(errno));
        fclose(src_fp);
        return false;
    }

    char buf[8192];
    size_t n;
    bool ok = true;

    while ((n = fread(buf, 1, sizeof(buf), src_fp)) > 0) {
        if (fwrite(buf, 1, n, dst_fp) != n) {
            LOG_ERROR("Backup: write failed to %s: %s", dst, strerror(errno));
            ok = false;
            break;
        }
    }

    fclose(src_fp);
    fclose(dst_fp);
    return ok;
}

/**
 * @brief Factory reset with backup (PROFINET reset mode 2).
 *
 * 1. Back up config file and database to /var/backup/water-treat/
 * 2. Delete originals (will regenerate defaults on restart)
 * 3. Clear p-net NV state
 * 4. SIGTERM to trigger clean shutdown (systemd restarts the service)
 */
static void factory_reset_with_backup(void) {
    /* Create backup directory */
    mkdir("/var/backup", 0755);
    mkdir(BACKUP_DIR, 0755);

    /* Timestamp for backup filenames */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[20];
    snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    bool backup_ok = true;

    /* Backup config file */
    const char *config_path = g_config_mgr.config_path;
    if (config_path[0] && access(config_path, R_OK) == 0) {
        char dst[MAX_PATH_LEN];
        snprintf(dst, sizeof(dst), "%s/config_%s.conf", BACKUP_DIR, ts);
        if (backup_copy_file(config_path, dst)) {
            LOG_INFO("Factory reset: config backed up to %s", dst);
        } else {
            backup_ok = false;
        }
    }

    /* Backup database */
    const char *db_path = g_app_config.database.path;
    if (db_path[0] && access(db_path, R_OK) == 0) {
        char dst[MAX_PATH_LEN];
        snprintf(dst, sizeof(dst), "%s/database_%s.db", BACKUP_DIR, ts);
        if (backup_copy_file(db_path, dst)) {
            LOG_INFO("Factory reset: database backed up to %s", dst);
        } else {
            backup_ok = false;
        }
    }

    if (!backup_ok) {
        LOG_ERROR("Factory reset: backup incomplete — aborting reset to protect data");
        return;
    }

    /* Delete originals so application regenerates defaults on restart */
    if (config_path[0] && access(config_path, F_OK) == 0) {
        if (remove(config_path) == 0) {
            LOG_INFO("Factory reset: removed %s", config_path);
        } else {
            LOG_ERROR("Factory reset: failed to remove %s: %s",
                      config_path, strerror(errno));
        }
    }

    if (db_path[0] && access(db_path, F_OK) == 0) {
        if (remove(db_path) == 0) {
            LOG_INFO("Factory reset: removed %s", db_path);
        } else {
            LOG_ERROR("Factory reset: failed to remove %s: %s",
                      db_path, strerror(errno));
        }
    }

    LOG_WARNING("Factory reset with backup complete — restarting application");

    /* SIGTERM triggers clean shutdown; systemd restarts the service */
    kill(getpid(), SIGTERM);
}

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
                /* Reset mode 2: Factory reset with backup
                 * Backs up config + database, then deletes originals so
                 * application regenerates defaults on systemd restart. */
                LOG_WARNING("Factory reset with backup requested");
                factory_reset_with_backup();
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

void profinet_callbacks_init(void) {
    /* No-op: I&M0 data only needed with real PROFINET stack */
}

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

    /* Handle config sync in stub mode (for testing) */
    if (idx == RTU_CONFIG_PROFINET_INDEX && data && len > 0) {
        LOG_INFO("Device config received (stub mode): %u bytes", len);
        config_sync_process_device(data, len);
    }
    if (idx == RTU_SENSOR_CONFIG_PROFINET_INDEX && data && len > 0) {
        LOG_INFO("Sensor config received (stub mode): %u bytes", len);
        config_sync_process_sensors(data, len);
    }
    if (idx == RTU_ACTUATOR_CONFIG_PROFINET_INDEX && data && len > 0) {
        LOG_INFO("Actuator config received (stub mode): %u bytes", len);
        config_sync_process_actuators(data, len);
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
