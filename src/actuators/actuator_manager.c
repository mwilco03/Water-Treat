/**
 * @file actuator_manager.c
 * @brief Actuator management with PROFINET output bridge
 */

#include "actuator_manager.h"
#include "profinet/profinet_manager.h"
#include "alarms/alarm_manager.h"
#include "db/db_events.h"
#include "db/db_modules.h"
#include "drivers/digital/relay_output.h"
#include "utils/logger.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define WATCHDOG_INTERVAL_MS    1000
#define COMMAND_TIMEOUT_MS      5000    // Consider disconnected if no command for 5s
#define DEGRADED_ALARM_DELAY_MS 3000    // Wait before declaring degraded mode

/* ============================================================================
 * Internal Structures
 * ========================================================================== */

// Alarm rule ID for degraded mode (created at runtime)
static int g_degraded_alarm_rule_id = -1;

/* ============================================================================
 * Internal Functions
 * ========================================================================== */

static actuator_instance_t* find_actuator_by_slot(actuator_manager_t *mgr, int slot) {
    for (int i = 0; i < mgr->actuator_count; i++) {
        if (mgr->actuators[i].config.profinet_slot == slot) {
            return &mgr->actuators[i];
        }
    }
    return NULL;
}

static result_t apply_actuator_state(actuator_instance_t *act) {
    output_driver_t *drv = (output_driver_t *)act->driver_handle;
    if (!drv) return RESULT_NOT_INITIALIZED;

    result_t r = RESULT_OK;

    if (act->state == ACTUATOR_STATE_ON) {
        if (act->config.pwm_capable && act->pwm_duty < 100) {
            r = output_set_pwm(drv, (float)act->pwm_duty / 100.0f);
        } else {
            r = output_set(drv, true);
        }
    } else {
        r = output_set(drv, false);
    }

    if (r == RESULT_OK) {
        act->last_state_change_ms = get_time_ms();
        act->cycle_count++;
        LOG_DEBUG("Actuator %s set to %s (PWM: %d%%)",
                  act->config.name,
                  act->state == ACTUATOR_STATE_ON ? "ON" : "OFF",
                  act->pwm_duty);
    } else {
        LOG_ERROR("Failed to set actuator %s state", act->config.name);
        act->state = ACTUATOR_STATE_FAULT;
    }

    return r;
}

static result_t init_actuator_driver(actuator_instance_t *act) {
    output_config_t cfg = {0};

    SAFE_STRNCPY(cfg.name, act->config.name, sizeof(cfg.name));
    cfg.gpio_pin = act->config.gpio_pin;
    cfg.active_low = act->config.active_low;

    switch (act->config.type) {
        case ACTUATOR_TYPE_PUMP:
            cfg.type = act->config.pwm_capable ? OUTPUT_TYPE_PWM : OUTPUT_TYPE_RELAY;
            cfg.pwm_frequency_hz = act->config.pwm_frequency_hz > 0 ?
                                   act->config.pwm_frequency_hz : 1000;
            break;
        case ACTUATOR_TYPE_VALVE:
            cfg.type = OUTPUT_TYPE_RELAY;
            break;
        default:
            cfg.type = OUTPUT_TYPE_RELAY;
            break;
    }

    cfg.max_on_time_sec = act->config.max_on_time_sec;

    output_driver_t *drv = NULL;
    result_t r = output_create(&drv, &cfg);
    if (r != RESULT_OK) {
        return r;
    }

    act->driver_handle = drv;

    LOG_INFO("Initialized actuator driver: %s (GPIO %d)",
             act->config.name, act->config.gpio_pin);

    return RESULT_OK;
}

static void destroy_actuator_driver(actuator_instance_t *act) {
    if (!act->driver_handle) return;

    // Ensure actuator is OFF before destroying
    act->state = ACTUATOR_STATE_OFF;
    apply_actuator_state(act);

    output_destroy((output_driver_t *)act->driver_handle);
    act->driver_handle = NULL;
}

static void enter_degraded_mode(actuator_manager_t *mgr) {
    if (mgr->degraded_mode) return;

    mgr->degraded_mode = true;
    mgr->disconnect_time_ms = get_time_ms();

    LOG_WARNING("Entering DEGRADED MODE - controller disconnected");
    LOG_WARNING("Actuators will maintain last commanded state");

    // Log event
    if (mgr->db) {
        DB_EVENT_WARNING(mgr->db, "actuator_manager",
                         "DEGRADED MODE: PROFINET controller disconnected, maintaining last state");
    }

    // Trigger callback
    if (mgr->on_degraded_mode) {
        mgr->on_degraded_mode(true, mgr->callback_ctx);
    }

    // Note: We do NOT change actuator states here
    // This implements "last-state-saved" behavior
}

static void exit_degraded_mode(actuator_manager_t *mgr) {
    if (!mgr->degraded_mode) return;

    mgr->degraded_mode = false;

    uint64_t degraded_duration_ms = get_time_ms() - mgr->disconnect_time_ms;

    LOG_INFO("Exiting DEGRADED MODE - controller reconnected (was degraded for %lu ms)",
             degraded_duration_ms);

    if (mgr->db) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "NORMAL MODE: PROFINET controller reconnected after %lu seconds",
                 degraded_duration_ms / 1000);
        DB_EVENT_INFO(mgr->db, "actuator_manager", msg);
    }

    // Trigger callback
    if (mgr->on_degraded_mode) {
        mgr->on_degraded_mode(false, mgr->callback_ctx);
    }
}

static void check_safety_limits(actuator_manager_t *mgr) {
    uint64_t now = get_time_ms();

    for (int i = 0; i < mgr->actuator_count; i++) {
        actuator_instance_t *act = &mgr->actuators[i];

        // Check max on time
        if (act->config.max_on_time_sec > 0 &&
            act->state == ACTUATOR_STATE_ON) {

            uint64_t on_duration_ms = now - act->last_state_change_ms;
            uint64_t max_on_ms = (uint64_t)act->config.max_on_time_sec * 1000;

            if (on_duration_ms >= max_on_ms) {
                LOG_WARNING("Actuator %s exceeded max on time (%d sec), forcing OFF",
                            act->config.name, act->config.max_on_time_sec);

                act->state = ACTUATOR_STATE_OFF;
                apply_actuator_state(act);

                if (mgr->db) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Safety shutoff: %s exceeded max on time",
                             act->config.name);
                    DB_EVENT_WARNING(mgr->db, "actuator_manager", msg);
                }
            }
        }
    }
}

static void* watchdog_thread(void *arg) {
    actuator_manager_t *mgr = (actuator_manager_t *)arg;

    LOG_INFO("Actuator watchdog thread started");

    while (mgr->running) {
        pthread_mutex_lock(&mgr->mutex);

        uint64_t now = get_time_ms();

        // Check for controller timeout (degraded mode detection)
        if (mgr->profinet_connected) {
            bool any_recent_command = false;

            for (int i = 0; i < mgr->actuator_count; i++) {
                if (mgr->actuators[i].last_command_time_ms > 0 &&
                    (now - mgr->actuators[i].last_command_time_ms) < COMMAND_TIMEOUT_MS) {
                    any_recent_command = true;
                    break;
                }
            }

            // If PROFINET says connected but no commands received, enter degraded
            // This catches the case where connection is stale
            if (!any_recent_command && mgr->actuator_count > 0) {
                // Only enter degraded mode after delay
                static uint64_t no_command_start = 0;
                if (no_command_start == 0) {
                    no_command_start = now;
                } else if ((now - no_command_start) > DEGRADED_ALARM_DELAY_MS) {
                    enter_degraded_mode(mgr);
                }
            }
        }

        // Check safety limits
        check_safety_limits(mgr);

        pthread_mutex_unlock(&mgr->mutex);

        usleep(WATCHDOG_INTERVAL_MS * 1000);
    }

    LOG_INFO("Actuator watchdog thread stopped");
    return NULL;
}

/* ============================================================================
 * PROFINET Callback Handlers
 * ========================================================================== */

// Global reference to manager for callbacks (single instance pattern)
static actuator_manager_t *g_actuator_mgr = NULL;

static void profinet_connect_handler(void *ctx) {
    actuator_manager_t *mgr = (actuator_manager_t *)ctx;
    if (!mgr) return;

    pthread_mutex_lock(&mgr->mutex);
    mgr->profinet_connected = true;
    exit_degraded_mode(mgr);
    pthread_mutex_unlock(&mgr->mutex);

    LOG_INFO("PROFINET controller connected");
}

static void profinet_disconnect_handler(void *ctx) {
    actuator_manager_t *mgr = (actuator_manager_t *)ctx;
    if (!mgr) return;

    pthread_mutex_lock(&mgr->mutex);
    mgr->profinet_connected = false;
    enter_degraded_mode(mgr);
    pthread_mutex_unlock(&mgr->mutex);

    LOG_WARNING("PROFINET controller disconnected");
}

static void profinet_output_handler(int slot, int subslot,
                                     const uint8_t *data, size_t len, void *ctx) {
    actuator_manager_t *mgr = (actuator_manager_t *)ctx;
    if (!mgr) return;

    actuator_manager_handle_output(mgr, slot, subslot, data, len);
}

/* ============================================================================
 * Public API
 * ========================================================================== */

result_t actuator_manager_init(actuator_manager_t *mgr, database_t *db) {
    CHECK_NULL(mgr);

    memset(mgr, 0, sizeof(*mgr));
    mgr->db = db;

    pthread_mutex_init(&mgr->mutex, NULL);

    mgr->initialized = true;
    g_actuator_mgr = mgr;

    LOG_INFO("Actuator manager initialized");
    return RESULT_OK;
}

result_t actuator_manager_start(actuator_manager_t *mgr) {
    CHECK_NULL(mgr);
    if (!mgr->initialized) return RESULT_NOT_INITIALIZED;
    if (mgr->running) return RESULT_OK;

    // Register PROFINET callbacks
    result_t r = profinet_manager_set_callbacks(
        profinet_connect_handler,
        profinet_disconnect_handler,
        profinet_output_handler,
        mgr
    );

    if (r != RESULT_OK) {
        LOG_WARNING("Failed to register PROFINET callbacks (PROFINET may be disabled)");
        // Continue anyway - can still use manual control
    }

    // Start watchdog thread
    mgr->running = true;
    if (pthread_create(&mgr->watchdog_thread, NULL, watchdog_thread, mgr) != 0) {
        LOG_ERROR("Failed to create watchdog thread");
        mgr->running = false;
        return RESULT_ERROR;
    }

    // Create degraded mode alarm rule if database available
    if (mgr->db && g_degraded_alarm_rule_id < 0) {
        // This alarm is special - it's triggered programmatically, not by threshold
        // We use module_id 0 as a virtual "system" module
        alarm_manager_create_rule(0, "DEGRADED_MODE", ALARM_CONDITION_ABOVE_THRESHOLD,
                                  1.0f, 0.0f, ALARM_SEVERITY_HIGH,
                                  &g_degraded_alarm_rule_id);
    }

    LOG_INFO("Actuator manager started with %d actuators", mgr->actuator_count);
    return RESULT_OK;
}

result_t actuator_manager_stop(actuator_manager_t *mgr) {
    CHECK_NULL(mgr);
    if (!mgr->running) return RESULT_OK;

    mgr->running = false;
    pthread_join(mgr->watchdog_thread, NULL);

    // Set all actuators to safe state (OFF)
    pthread_mutex_lock(&mgr->mutex);
    for (int i = 0; i < mgr->actuator_count; i++) {
        mgr->actuators[i].state = ACTUATOR_STATE_OFF;
        apply_actuator_state(&mgr->actuators[i]);
    }
    pthread_mutex_unlock(&mgr->mutex);

    LOG_INFO("Actuator manager stopped");
    return RESULT_OK;
}

void actuator_manager_destroy(actuator_manager_t *mgr) {
    if (!mgr) return;

    actuator_manager_stop(mgr);

    pthread_mutex_lock(&mgr->mutex);
    for (int i = 0; i < mgr->actuator_count; i++) {
        destroy_actuator_driver(&mgr->actuators[i]);
    }
    mgr->actuator_count = 0;
    pthread_mutex_unlock(&mgr->mutex);

    pthread_mutex_destroy(&mgr->mutex);

    mgr->initialized = false;
    g_actuator_mgr = NULL;

    LOG_INFO("Actuator manager destroyed");
}

result_t actuator_manager_add(actuator_manager_t *mgr, const actuator_config_t *config) {
    CHECK_NULL(mgr); CHECK_NULL(config);
    if (!mgr->initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&mgr->mutex);

    if (mgr->actuator_count >= MAX_ACTUATORS) {
        pthread_mutex_unlock(&mgr->mutex);
        LOG_ERROR("Maximum actuators reached");
        return RESULT_NO_MEMORY;
    }

    // Check for duplicate slot
    if (find_actuator_by_slot(mgr, config->profinet_slot)) {
        pthread_mutex_unlock(&mgr->mutex);
        LOG_ERROR("Actuator already exists at slot %d", config->profinet_slot);
        return RESULT_ALREADY_EXISTS;
    }

    actuator_instance_t *act = &mgr->actuators[mgr->actuator_count];
    memset(act, 0, sizeof(*act));
    memcpy(&act->config, config, sizeof(actuator_config_t));

    // Initialize driver
    result_t r = init_actuator_driver(act);
    if (r != RESULT_OK) {
        pthread_mutex_unlock(&mgr->mutex);
        return r;
    }

    // Register with PROFINET manager as output module
    if (profinet_manager_is_running()) {
        profinet_manager_add_module(NULL,
                                     config->profinet_slot,
                                     0x00000002,  // Output module ident
                                     config->profinet_subslot,
                                     0x00000002,  // Output submodule ident
                                     0,           // No input data
                                     sizeof(actuator_output_data_t));  // Output data size
    }

    mgr->actuator_count++;

    pthread_mutex_unlock(&mgr->mutex);

    LOG_INFO("Added actuator: %s (slot %d, GPIO %d, type %d)",
             config->name, config->profinet_slot, config->gpio_pin, config->type);

    return RESULT_OK;
}

result_t actuator_manager_remove(actuator_manager_t *mgr, int profinet_slot) {
    CHECK_NULL(mgr);
    if (!mgr->initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&mgr->mutex);

    for (int i = 0; i < mgr->actuator_count; i++) {
        if (mgr->actuators[i].config.profinet_slot == profinet_slot) {
            destroy_actuator_driver(&mgr->actuators[i]);

            // Shift remaining actuators
            for (int j = i; j < mgr->actuator_count - 1; j++) {
                mgr->actuators[j] = mgr->actuators[j + 1];
            }
            mgr->actuator_count--;

            pthread_mutex_unlock(&mgr->mutex);
            LOG_INFO("Removed actuator at slot %d", profinet_slot);
            return RESULT_OK;
        }
    }

    pthread_mutex_unlock(&mgr->mutex);
    return RESULT_NOT_FOUND;
}

result_t actuator_manager_handle_output(actuator_manager_t *mgr,
                                         int slot, int subslot,
                                         const uint8_t *data, size_t len) {
    CHECK_NULL(mgr); CHECK_NULL(data);
    UNUSED(subslot);

    if (!mgr->initialized) return RESULT_NOT_INITIALIZED;
    if (len < sizeof(actuator_output_data_t)) {
        LOG_WARNING("Invalid output data length: %zu", len);
        return RESULT_INVALID_PARAM;
    }

    pthread_mutex_lock(&mgr->mutex);

    actuator_instance_t *act = find_actuator_by_slot(mgr, slot);
    if (!act) {
        pthread_mutex_unlock(&mgr->mutex);
        LOG_DEBUG("No actuator at slot %d", slot);
        return RESULT_NOT_FOUND;
    }

    // Parse output data
    const actuator_output_data_t *cmd = (const actuator_output_data_t *)data;

    actuator_state_t new_state;
    uint8_t new_pwm = cmd->pwm_duty;

    switch (cmd->command) {
        case ACTUATOR_CMD_OFF:
            new_state = ACTUATOR_STATE_OFF;
            new_pwm = 0;
            break;
        case ACTUATOR_CMD_ON:
            new_state = ACTUATOR_STATE_ON;
            new_pwm = 100;
            break;
        case ACTUATOR_CMD_PWM:
            new_state = ACTUATOR_STATE_ON;
            if (new_pwm > 100) new_pwm = 100;
            break;
        default:
            pthread_mutex_unlock(&mgr->mutex);
            LOG_WARNING("Unknown actuator command: 0x%02X", cmd->command);
            return RESULT_INVALID_PARAM;
    }

    // Check for minimum cycle time
    if (act->config.min_cycle_time_ms > 0) {
        uint64_t elapsed = get_time_ms() - act->last_state_change_ms;
        if (elapsed < (uint64_t)act->config.min_cycle_time_ms) {
            pthread_mutex_unlock(&mgr->mutex);
            LOG_DEBUG("Actuator %s cycle too fast, ignoring", act->config.name);
            return RESULT_OK;
        }
    }

    // Update command tracking
    act->last_command_time_ms = get_time_ms();
    act->controller_connected = true;
    act->last_commanded_state = new_state;

    // Apply state if changed
    if (act->state != new_state || act->pwm_duty != new_pwm) {
        act->state = new_state;
        act->pwm_duty = new_pwm;
        act->manual_mode = false;  // PROFINET command clears manual mode
        apply_actuator_state(act);

        LOG_DEBUG("Actuator %s: command=%d, state=%d, PWM=%d%%",
                  act->config.name, cmd->command, new_state, new_pwm);
    }

    // Exit degraded mode if we were in it
    if (mgr->degraded_mode) {
        exit_degraded_mode(mgr);
    }

    pthread_mutex_unlock(&mgr->mutex);
    return RESULT_OK;
}

result_t actuator_manager_set_connected(actuator_manager_t *mgr, bool connected) {
    CHECK_NULL(mgr);

    pthread_mutex_lock(&mgr->mutex);

    mgr->profinet_connected = connected;

    if (connected) {
        exit_degraded_mode(mgr);
    } else {
        enter_degraded_mode(mgr);
    }

    pthread_mutex_unlock(&mgr->mutex);
    return RESULT_OK;
}

result_t actuator_manager_get_state(actuator_manager_t *mgr, int slot,
                                     actuator_state_t *state, uint8_t *pwm_duty) {
    CHECK_NULL(mgr);

    pthread_mutex_lock(&mgr->mutex);

    actuator_instance_t *act = find_actuator_by_slot(mgr, slot);
    if (!act) {
        pthread_mutex_unlock(&mgr->mutex);
        return RESULT_NOT_FOUND;
    }

    if (state) *state = act->state;
    if (pwm_duty) *pwm_duty = act->pwm_duty;

    pthread_mutex_unlock(&mgr->mutex);
    return RESULT_OK;
}

result_t actuator_manager_get_full_state(actuator_manager_t *mgr, int slot,
                                          actuator_state_t *state, uint8_t *pwm_duty,
                                          bool *manual_mode) {
    CHECK_NULL(mgr);

    pthread_mutex_lock(&mgr->mutex);

    actuator_instance_t *act = find_actuator_by_slot(mgr, slot);
    if (!act) {
        pthread_mutex_unlock(&mgr->mutex);
        return RESULT_NOT_FOUND;
    }

    if (state) *state = act->state;
    if (pwm_duty) *pwm_duty = act->pwm_duty;
    if (manual_mode) *manual_mode = act->manual_mode;

    pthread_mutex_unlock(&mgr->mutex);
    return RESULT_OK;
}

result_t actuator_manager_manual_set(actuator_manager_t *mgr, int slot,
                                      actuator_state_t state, uint8_t pwm_duty) {
    CHECK_NULL(mgr);
    if (!mgr->initialized) return RESULT_NOT_INITIALIZED;

    pthread_mutex_lock(&mgr->mutex);

    actuator_instance_t *act = find_actuator_by_slot(mgr, slot);
    if (!act) {
        pthread_mutex_unlock(&mgr->mutex);
        return RESULT_NOT_FOUND;
    }

    act->state = state;
    act->pwm_duty = pwm_duty;
    act->manual_mode = true;  // Mark as manual control (TUI override)
    result_t r = apply_actuator_state(act);

    if (r == RESULT_OK && mgr->db) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Manual control: %s set to %s",
                 act->config.name,
                 state == ACTUATOR_STATE_ON ? "ON" : "OFF");
        DB_EVENT_INFO(mgr->db, "actuator_manager", msg);
    }

    pthread_mutex_unlock(&mgr->mutex);
    return r;
}

result_t actuator_manager_emergency_stop(actuator_manager_t *mgr) {
    CHECK_NULL(mgr);

    pthread_mutex_lock(&mgr->mutex);

    LOG_WARNING("EMERGENCY STOP - all actuators");

    for (int i = 0; i < mgr->actuator_count; i++) {
        mgr->actuators[i].state = ACTUATOR_STATE_OFF;
        mgr->actuators[i].pwm_duty = 0;
        apply_actuator_state(&mgr->actuators[i]);
    }

    if (mgr->db) {
        DB_EVENT_ERROR(mgr->db, "actuator_manager", "EMERGENCY STOP activated");
    }

    pthread_mutex_unlock(&mgr->mutex);
    return RESULT_OK;
}

result_t actuator_manager_set_callback(actuator_manager_t *mgr,
                                        void (*on_degraded)(bool, void*),
                                        void *ctx) {
    CHECK_NULL(mgr);

    pthread_mutex_lock(&mgr->mutex);
    mgr->on_degraded_mode = on_degraded;
    mgr->callback_ctx = ctx;
    pthread_mutex_unlock(&mgr->mutex);

    return RESULT_OK;
}

bool actuator_manager_is_degraded(actuator_manager_t *mgr) {
    if (!mgr) return false;
    return mgr->degraded_mode;
}

int actuator_manager_get_count(actuator_manager_t *mgr) {
    if (!mgr) return 0;
    return mgr->actuator_count;
}

result_t actuator_manager_reload(actuator_manager_t *mgr) {
    CHECK_NULL(mgr);
    if (!mgr->initialized || !mgr->db) return RESULT_NOT_INITIALIZED;

    LOG_INFO("Loading actuators from database...");

    /* Query all actuators from database */
    db_actuator_t *db_actuators = NULL;
    int db_count = 0;

    result_t r = db_actuator_list(mgr->db, &db_actuators, &db_count);
    if (r != RESULT_OK) {
        LOG_ERROR("Failed to query actuators from database");
        return r;
    }

    if (db_count == 0) {
        LOG_INFO("No actuators configured in database");
        if (db_actuators) free(db_actuators);
        return RESULT_OK;
    }

    LOG_INFO("Found %d actuators in database", db_count);

    int added = 0;
    int skipped = 0;

    for (int i = 0; i < db_count; i++) {
        db_actuator_t *db_act = &db_actuators[i];

        /* Skip disabled actuators */
        if (!db_act->enabled) {
            LOG_DEBUG("Skipping disabled actuator: %s", db_act->name);
            skipped++;
            continue;
        }

        /* Check if actuator already exists at this slot */
        if (find_actuator_by_slot(mgr, db_act->slot)) {
            LOG_DEBUG("Actuator already loaded at slot %d, skipping", db_act->slot);
            skipped++;
            continue;
        }

        /* Convert db_actuator_t to actuator_config_t */
        actuator_config_t config = {0};
        config.id = db_act->id;
        SAFE_STRNCPY(config.name, db_act->name, sizeof(config.name));
        config.type = db_act->type;
        config.profinet_slot = db_act->slot;
        config.profinet_subslot = db_act->subslot > 0 ? db_act->subslot : 1;
        config.gpio_pin = db_act->gpio_pin;
        config.active_low = db_act->active_low;
        config.pwm_capable = (db_act->type == ACTUATOR_TYPE_PWM ||
                              db_act->type == ACTUATOR_TYPE_PUMP);
        config.pwm_frequency_hz = db_act->pwm_frequency_hz;
        config.max_on_time_sec = db_act->max_on_time_ms / 1000;
        config.min_cycle_time_ms = db_act->min_on_time_ms;

        /* Add the actuator */
        r = actuator_manager_add(mgr, &config);
        if (r == RESULT_OK) {
            LOG_INFO("Loaded actuator: %s (slot %d, GPIO %d)",
                     config.name, config.profinet_slot, config.gpio_pin);
            added++;
        } else {
            LOG_WARNING("Failed to add actuator %s: %d", config.name, r);
        }
    }

    free(db_actuators);

    LOG_INFO("Actuator reload complete: %d added, %d skipped", added, skipped);
    return RESULT_OK;
}
