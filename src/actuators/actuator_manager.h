/**
 * @file actuator_manager.h
 * @brief Actuator management with PROFINET output bridge
 *
 * Bridges PROFINET output data to physical actuators (pumps, valves).
 * Handles offline autonomy with last-state-saved behavior.
 */

#ifndef ACTUATOR_MANAGER_H
#define ACTUATOR_MANAGER_H

#include "common.h"
#include "db/database.h"
#include "db/db_actuators.h"

/* ============================================================================
 * Actuator State (runtime, not persisted type)
 * ========================================================================== */

/* actuator_type_t is defined in db/db_actuators.h */

typedef enum {
    ACTUATOR_STATE_OFF = 0,
    ACTUATOR_STATE_ON = 1,
    ACTUATOR_STATE_FAULT = -1,
} actuator_state_t;

/* ============================================================================
 * PROFINET Output Data Format
 * ========================================================================== */

/**
 * PROFINET output data format for actuator control
 * Each actuator slot receives 4 bytes:
 *   Byte 0: Command (0=OFF, 1=ON, 2=PWM)
 *   Byte 1: PWM duty cycle (0-100) if command=2
 *   Byte 2: Reserved
 *   Byte 3: Reserved
 */
#define ACTUATOR_CMD_OFF    0x00
#define ACTUATOR_CMD_ON     0x01
#define ACTUATOR_CMD_PWM    0x02

typedef struct {
    uint8_t command;
    uint8_t pwm_duty;
    uint8_t reserved[2];
} __attribute__((packed)) actuator_output_data_t;

/* ============================================================================
 * Actuator Configuration
 * ========================================================================== */

#define MAX_ACTUATORS 16

typedef struct {
    int id;
    char name[MAX_NAME_LEN];
    actuator_type_t type;

    // PROFINET mapping
    int profinet_slot;
    int profinet_subslot;

    // GPIO configuration
    int gpio_pin;
    bool active_low;

    // PWM settings (for pumps with variable speed)
    bool pwm_capable;
    int pwm_frequency_hz;

    // Safety limits
    int max_on_time_sec;        // Auto-shutoff (0=disabled)
    int min_cycle_time_ms;      // Prevent rapid cycling

} actuator_config_t;

/* ============================================================================
 * Actuator Instance
 * ========================================================================== */

typedef struct {
    actuator_config_t config;
    actuator_state_t state;
    actuator_state_t last_commanded_state;  // Last state from controller
    uint8_t pwm_duty;

    // Timing
    uint64_t last_state_change_ms;
    uint64_t total_on_time_ms;
    int cycle_count;

    // Connection tracking
    bool controller_connected;
    uint64_t last_command_time_ms;

    // Driver handle
    void *driver_handle;

} actuator_instance_t;

/* ============================================================================
 * Actuator Manager
 * ========================================================================== */

typedef struct {
    database_t *db;

    actuator_instance_t actuators[MAX_ACTUATORS];
    int actuator_count;

    // PROFINET connection state
    bool profinet_connected;
    bool degraded_mode;
    uint64_t disconnect_time_ms;

    // Threading
    pthread_mutex_t mutex;
    pthread_t watchdog_thread;
    volatile bool running;
    bool initialized;

    // Callbacks
    void (*on_degraded_mode)(bool degraded, void *ctx);
    void *callback_ctx;

} actuator_manager_t;

/* ============================================================================
 * Public API
 * ========================================================================== */

/**
 * Initialize actuator manager
 */
result_t actuator_manager_init(actuator_manager_t *mgr, database_t *db);

/**
 * Start actuator manager (registers PROFINET callbacks)
 */
result_t actuator_manager_start(actuator_manager_t *mgr);

/**
 * Stop actuator manager
 */
result_t actuator_manager_stop(actuator_manager_t *mgr);

/**
 * Shutdown and cleanup
 */
void actuator_manager_destroy(actuator_manager_t *mgr);

/**
 * Add an actuator instance
 */
result_t actuator_manager_add(actuator_manager_t *mgr, const actuator_config_t *config);

/**
 * Remove an actuator by slot
 */
result_t actuator_manager_remove(actuator_manager_t *mgr, int profinet_slot);

/**
 * Handle PROFINET output data (called from PROFINET callbacks)
 * This is the bridge function that dispatches commands to actuators
 */
result_t actuator_manager_handle_output(actuator_manager_t *mgr,
                                         int slot, int subslot,
                                         const uint8_t *data, size_t len);

/**
 * Handle PROFINET connection state change
 */
result_t actuator_manager_set_connected(actuator_manager_t *mgr, bool connected);

/**
 * Get actuator state
 */
result_t actuator_manager_get_state(actuator_manager_t *mgr, int slot,
                                     actuator_state_t *state, uint8_t *pwm_duty);

/**
 * Manual control (for TUI/testing) - bypasses PROFINET
 */
result_t actuator_manager_manual_set(actuator_manager_t *mgr, int slot,
                                      actuator_state_t state, uint8_t pwm_duty);

/**
 * Emergency stop all actuators
 */
result_t actuator_manager_emergency_stop(actuator_manager_t *mgr);

/**
 * Set degraded mode callback
 */
result_t actuator_manager_set_callback(actuator_manager_t *mgr,
                                        void (*on_degraded)(bool, void*),
                                        void *ctx);

/**
 * Check if in degraded mode (controller disconnected)
 */
bool actuator_manager_is_degraded(actuator_manager_t *mgr);

/**
 * Get actuator count
 */
int actuator_manager_get_count(actuator_manager_t *mgr);

/**
 * Reload actuator configuration from database
 */
result_t actuator_manager_reload(actuator_manager_t *mgr);

#endif /* ACTUATOR_MANAGER_H */
