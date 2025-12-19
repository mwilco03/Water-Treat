#ifndef DB_ACTUATORS_H
#define DB_ACTUATORS_H

#include "common.h"
#include "database.h"

/**
 * Actuator type definitions for PROFINET output modules
 */
typedef enum {
    ACTUATOR_TYPE_RELAY = 0,    // Simple on/off relay
    ACTUATOR_TYPE_PWM,          // PWM output for variable speed
    ACTUATOR_TYPE_LATCHING,     // Latching relay (maintains state)
    ACTUATOR_TYPE_MOMENTARY,    // Momentary contact
    ACTUATOR_TYPE_PUMP,         // Pump (may support PWM speed control)
    ACTUATOR_TYPE_VALVE         // Valve (solenoid or motorized)
} actuator_type_t;

/**
 * Actuator safe state on controller disconnect
 */
typedef enum {
    SAFE_STATE_OFF = 0,         // Turn off on disconnect
    SAFE_STATE_ON,              // Turn on on disconnect
    SAFE_STATE_HOLD             // Maintain last state (last-state-saved)
} safe_state_t;

/**
 * Actuator database record
 */
typedef struct {
    int id;
    int slot;                       // PROFINET output slot (9-15)
    int subslot;
    char name[MAX_NAME_LEN];
    actuator_type_t type;
    int gpio_pin;                   // GPIO pin number
    char gpio_chip[32];             // GPIO chip (e.g., "gpiochip0")
    bool active_low;                // Invert logic
    safe_state_t safe_state;        // State on controller disconnect
    int min_on_time_ms;             // Minimum on time (anti-short-cycle)
    int max_on_time_ms;             // Maximum on time (safety timeout)
    int pwm_frequency_hz;           // PWM frequency (for PWM type)
    char status[16];                // Current status
    bool enabled;
} db_actuator_t;

/**
 * Actuator runtime state (for persistence)
 */
typedef struct {
    int actuator_id;
    bool state;                     // Current on/off state
    int pwm_duty;                   // Current PWM duty cycle (0-255)
    uint64_t last_state_change;     // Timestamp of last change
    uint64_t total_on_time_ms;      // Total accumulated on time
    int cycle_count;                // Number of on/off cycles
} db_actuator_state_t;

// Actuator CRUD operations
result_t db_actuator_create(database_t *db, db_actuator_t *actuator, int *actuator_id);
result_t db_actuator_update(database_t *db, db_actuator_t *actuator);
result_t db_actuator_delete(database_t *db, int actuator_id);
result_t db_actuator_get(database_t *db, int actuator_id, db_actuator_t *actuator);
result_t db_actuator_get_by_slot(database_t *db, int slot, db_actuator_t *actuator);
result_t db_actuator_list(database_t *db, db_actuator_t **actuators, int *count);
result_t db_actuator_count(database_t *db, int *count);

// Actuator state operations
result_t db_actuator_state_update(database_t *db, int actuator_id, bool state, int pwm_duty);
result_t db_actuator_state_get(database_t *db, int actuator_id, db_actuator_state_t *state);
result_t db_actuator_state_increment_cycle(database_t *db, int actuator_id);

// GPIO pin conflict detection
typedef struct {
    bool has_conflict;
    int conflict_type;              // 0 = actuator, 1 = sensor
    int conflicting_actuator_id;    // ID of conflicting actuator (or -1 for sensor)
    char conflicting_name[MAX_NAME_LEN];
} gpio_conflict_t;

/**
 * Check if GPIO pin is already used by another actuator
 * @param db Database handle
 * @param gpio_pin GPIO pin number
 * @param gpio_chip GPIO chip name
 * @param exclude_actuator_id Actuator ID to exclude from check (0 for new)
 * @param conflict Output conflict information
 * @return RESULT_OK on success
 */
result_t db_actuator_gpio_conflict_check(database_t *db, int gpio_pin,
                                         const char *gpio_chip,
                                         int exclude_actuator_id,
                                         gpio_conflict_t *conflict);

// Utility
void db_actuator_free_list(db_actuator_t *actuators);
const char* actuator_type_to_string(actuator_type_t type);
const char* safe_state_to_string(safe_state_t state);

#endif
