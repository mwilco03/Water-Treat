#ifndef PROFINET_MANAGER_H
#define PROFINET_MANAGER_H

#include "common.h"
#include "db/database.h"
#include "config/config.h"

#ifdef HAVE_PNET
#include <pnet_api.h>
#endif

typedef enum {
    PROFINET_STATE_IDLE = 0,
    PROFINET_STATE_READY,
    PROFINET_STATE_CONNECTING,
    PROFINET_STATE_CONNECTED,
    PROFINET_STATE_ERROR
} profinet_state_t;

typedef struct {
    profinet_state_t state;
    bool connected;
    uint32_t cycle_count;
    int slot_count;
    int plugged_modules;

    /* Connection resilience statistics */
    uint32_t connection_count;       /**< Total successful connections */
    uint32_t disconnect_count;       /**< Total disconnections */
    uint32_t error_count;            /**< Total recoverable errors */
    uint32_t stuck_state_recoveries; /**< Times recovered from stuck state */
    uint64_t state_duration_ms;      /**< Time in current state (ms) */

    /* Output polling efficiency (change detection) */
    uint64_t output_polls;           /**< Total output slot polls */
    uint64_t output_changes;         /**< Polls that detected actual changes */
} profinet_stats_t;

/**
 * Public slot information for HTTP API
 * Contains only fields needed for /slots endpoint
 */
typedef struct {
    int slot;
    int subslot;
    uint32_t module_ident;
    uint32_t submodule_ident;
    size_t input_size;
    size_t output_size;
} profinet_slot_info_t;

// PROFINET IOXS values (only define when p-net is not available)
// When HAVE_PNET is defined, these come from pnet_api.h as enum values
#ifndef HAVE_PNET
#define PNET_IOXS_BAD  0x00
#define PNET_IOXS_GOOD 0x80
#endif

typedef void (*profinet_connect_cb_t)(void *ctx);
typedef void (*profinet_disconnect_cb_t)(void *ctx);
typedef void (*profinet_data_cb_t)(int slot, int subslot, const uint8_t *data, size_t len, void *ctx);

result_t profinet_manager_init(database_t *db, const profinet_config_t *config);
result_t profinet_manager_start(const char *interface);

/**
 * @brief Mark PROFINET as disabled by configuration
 *
 * Called by main.c when profinet.enabled=false in config.
 * Distinguishes "disabled by config" from "failed to initialize".
 */
void profinet_manager_mark_disabled(void);
result_t profinet_manager_stop(void);
void profinet_manager_shutdown(void);

result_t profinet_manager_update_input(int slot, int subslot, const void *data, size_t size);
result_t profinet_manager_update_input_float(int slot, int subslot, float value);

/**
 * @brief Update PROFINET input with value and quality (5-byte format)
 *
 * Per DEVELOPMENT_GUIDELINES.md Part 1.2, sensor data is transmitted as:
 *   Bytes 0-3: Float32 value (big-endian)
 *   Byte 4:    Quality indicator (OPC UA compatible)
 *
 * @param slot     PROFINET slot number
 * @param subslot  PROFINET subslot number
 * @param value    Sensor value in engineering units
 * @param quality  Data quality indicator
 * @return RESULT_OK on success
 */
result_t profinet_manager_update_input_with_quality(int slot, int subslot,
                                                     float value, data_quality_t quality);

result_t profinet_manager_get_output(int slot, int subslot, void *data, size_t *size);

result_t profinet_manager_write_input_data(void *mgr, int slot, int subslot, const uint8_t *data, size_t len);
result_t profinet_manager_set_input_iops(void *mgr, int slot, int subslot, uint8_t iops);
result_t profinet_manager_add_module(void *mgr, int slot, uint32_t module_ident, int subslot,
                                      uint32_t submodule_ident, size_t input_len, size_t output_len);

result_t profinet_manager_set_callbacks(profinet_connect_cb_t on_connect,
                                        profinet_disconnect_cb_t on_disconnect,
                                        profinet_data_cb_t on_data, void *ctx);

profinet_state_t profinet_manager_get_state(void);
bool profinet_manager_is_connected(void);
bool profinet_manager_is_running(void);
result_t profinet_manager_get_stats(profinet_stats_t *stats);

/**
 * @brief Get list of all plugged PROFINET slots (database + runtime)
 *
 * Returns ALL slots registered with PROFINET manager, including:
 * - Database-configured sensors/actuators
 * - Runtime-created sensors (e.g., CPU temperature at slot 1)
 *
 * @param slots Output array pointer (caller must free)
 * @param count Output slot count
 * @return RESULT_OK on success, RESULT_NOT_INITIALIZED if PROFINET not started
 */
result_t profinet_manager_get_slot_list(profinet_slot_info_t **slots, int *count);

result_t profinet_manager_send_alarm(int slot, int subslot, uint16_t alarm_type,
                                     const uint8_t *data, size_t data_len);

const char* profinet_state_to_string(profinet_state_t state);

/**
 * @brief Get PROFINET initialization error message
 *
 * Returns a detailed error message if PROFINET failed to initialize.
 * Used by health check module to provide actionable diagnostics.
 *
 * @return Error message string, or NULL if no error
 */
const char* profinet_manager_get_init_error(void);

/**
 * @brief Check if PROFINET was disabled by configuration
 *
 * @return true if PROFINET was disabled in config file
 */
bool profinet_manager_is_disabled_by_config(void);

/**
 * @brief Check if PROFINET initialization was attempted
 *
 * Distinguishes between:
 * - PROFINET disabled by config (init not attempted)
 * - PROFINET enabled but init failed (init attempted)
 * - PROFINET library not compiled in (HAVE_PNET not defined)
 *
 * @return true if pnet_init() was called (regardless of success/failure)
 */
bool profinet_manager_init_attempted(void);

/**
 * @brief Build binary slot map for PROFINET Record Read 0xF844
 *
 * Serializes current slot configuration into big-endian packed binary
 * for the controller's PROFINET-only fallback discovery (step 5).
 *
 * Wire format (all multi-byte fields big-endian):
 *   Bytes 0-1:  uint16_t slot_count (application slots, excludes DAP)
 *   Per slot (15 bytes each):
 *     uint16_t slot_number
 *     uint16_t subslot_number
 *     uint32_t module_ident
 *     uint32_t submodule_ident
 *     uint8_t  direction (1=input, 2=output)
 *     uint16_t data_size (5 for sensors, 4 for actuators)
 *
 * @param buffer      Output buffer (min 2 + slot_count * 15 bytes)
 * @param buffer_size Buffer capacity
 * @return Bytes written on success, 0 if no slots configured, -1 on error
 */
int profinet_manager_build_slot_map(uint8_t *buffer, size_t buffer_size);

// Internal callbacks used by profinet_callbacks.c
void profinet_manager_set_connected(bool connected, uint32_t arep);
void profinet_manager_handle_output_data(int slot, int subslot, const uint8_t *data, size_t len);

/**
 * @brief Look up plugged module ident for a slot (any subslot)
 *
 * Used by exp_module_callback to validate the controller's expected
 * module ident against what the RTU actually plugged.
 *
 * @param slot          Slot number to look up
 * @param module_ident  Output: module ident if found (may be NULL)
 * @return true if a module is plugged in this slot
 */
bool profinet_manager_get_plugged_module_ident(int slot, uint32_t *module_ident);

/**
 * @brief Look up plugged submodule ident for a slot+subslot
 *
 * Used by exp_submodule_callback to validate the controller's expected
 * submodule ident against what the RTU actually plugged.
 *
 * @param slot             Slot number
 * @param subslot          Subslot number
 * @param module_ident     Output: module ident if found (may be NULL)
 * @param submodule_ident  Output: submodule ident if found (may be NULL)
 * @return true if a submodule is plugged at this slot+subslot
 */
bool profinet_manager_get_plugged_submodule_ident(int slot, int subslot,
                                                   uint32_t *module_ident,
                                                   uint32_t *submodule_ident);

/**
 * @brief Clear stale AR state to recover from connection errors
 *
 * Auto-recovery for PNIO errors 0x03 (AR exists) and 0x04 (session mismatch).
 * Clears NV files and resets state so controller can reconnect.
 */
void profinet_manager_clear_ar_state(void);

/**
 * @brief Initialize all input subslots with default data and GOOD IOPS
 *
 * CRITICAL: This function MUST be called before pnet_application_ready().
 * The p-net library requires all input subslots to have valid data and
 * IOPS set before the application can signal readiness to the controller.
 *
 * Called from profinet_state_callback() on PNET_EVENT_PRMEND.
 *
 * @return Number of input subslots initialized
 */
int profinet_manager_init_all_inputs(void);

/**
 * @brief Dump all plugged slots to log for debugging
 *
 * Logs comprehensive slot configuration including:
 * - DAP subslots (slot 0)
 * - All application modules with idents, directions, sizes
 * - Current IOPS status per slot
 *
 * Call after profinet_manager_start() to verify what's registered.
 * Useful for diagnosing IOCR mismatch errors from controller.
 */
void profinet_manager_dump_slots(void);

/**
 * @brief Clear all application module slots (not DAP)
 *
 * Must be called before re-adding modules during reload to prevent
 * stale slots from persisting. Only clears slot tracking; does not
 * unplug from p-net (which requires a connection reset).
 */
void profinet_manager_clear_app_slots(void);

/**
 * @brief Remove a specific application module slot
 *
 * Targeted removal for individual slot cleanup (e.g., when a sensor is
 * deleted). Safer than clear_app_slots when only one subsystem reloads.
 *
 * @param slot     Slot number to remove (must be > 0; DAP cannot be removed)
 * @param subslot  Subslot number
 */
void profinet_manager_remove_slot(int slot, int subslot);

/**
 * @brief Send PROFINET channel diagnosis alarm for a sensor submodule
 *
 * Per IEC 61158-6-10, sends a standard channel diagnosis alarm when a
 * sensor transitions to BAD/NOT_CONNECTED, or clears the alarm when the
 * sensor recovers to GOOD/UNCERTAIN.
 *
 * Uses p-net diagnosis API (pnet_diag_add/remove) which:
 *   - Stores diagnosis state in the IO-Device (per PROFINET spec)
 *   - Automatically sends alarm type 0x0001 (appears) when diagnosis added
 *   - Automatically sends alarm type 0x0002 (disappears) when diagnosis removed
 *
 * Wire-compliant: uses standard diagnosis alarm types and USI 0x8000.
 *
 * @param slot      PROFINET slot number
 * @param subslot   PROFINET subslot number
 * @param quality   Current data quality
 * @return RESULT_OK on success
 */
result_t profinet_manager_send_diagnosis(int slot, int subslot, data_quality_t quality);

/**
 * @brief Notify that controller cyclic data RUN bit has gone to 0
 *
 * Called from data status callback.  Triggers actuator disconnect handlers
 * so they can enter safe state.
 */
void profinet_manager_on_data_run_stop(void);

/**
 * @brief Get the current Application Relationship Endpoint (AREP)
 *
 * @return AREP value, or 0 if not connected
 */
uint32_t profinet_manager_get_arep(void);

/**
 * @brief Set the controller-provided watchdog timeout
 *
 * Called by config_sync when the controller sends a device config (0xF841)
 * that includes a watchdog_ms field.  This timeout overrides the default
 * liveness check interval.
 *
 * @param watchdog_ms  Timeout in milliseconds (0 = use default)
 */
void profinet_manager_set_controller_watchdog(uint32_t watchdog_ms);

#endif
