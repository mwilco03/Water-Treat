/**
 * @file alarm_manager.c
 * @brief Alarm management system
 */

#include "alarm_manager.h"
#include "db/db_alarms.h"
#include "db/db_events.h"
#include "db/db_modules.h"
#include "actuators/actuator_manager.h"
#include "utils/logger.h"
#include <pthread.h>
#include <math.h>

/* External actuator manager for safety interlocks */
extern actuator_manager_t g_actuator_mgr;

#define MAX_ALARM_RULES 256
#define ALARM_CHECK_INTERVAL_MS 1000

typedef struct {
    int rule_id;
    float last_value;
    bool in_alarm;
    int active_alarm_id;
    uint64_t last_check_time;
    float rate_buffer[10];
    int rate_buffer_idx;
} alarm_rule_state_t;

typedef struct {
    database_t *db;
    alarm_rule_state_t states[MAX_ALARM_RULES];
    int state_count;
    
    pthread_t check_thread;
    pthread_mutex_t mutex;
    volatile bool running;
    bool initialized;
    
    alarm_callback_t on_alarm_raised;
    alarm_callback_t on_alarm_cleared;
    void *callback_ctx;
} alarm_manager_t;

static alarm_manager_t g_alarm_mgr = {0};

/* ============================================================================
 * Internal Functions
 * ========================================================================== */

static alarm_rule_state_t* find_state(int rule_id) {
    for (int i = 0; i < g_alarm_mgr.state_count; i++) {
        if (g_alarm_mgr.states[i].rule_id == rule_id) return &g_alarm_mgr.states[i];
    }
    return NULL;
}

static alarm_rule_state_t* get_or_create_state(int rule_id) {
    alarm_rule_state_t *state = find_state(rule_id);
    if (state) return state;
    if (g_alarm_mgr.state_count >= MAX_ALARM_RULES) return NULL;
    
    state = &g_alarm_mgr.states[g_alarm_mgr.state_count++];
    memset(state, 0, sizeof(*state));
    state->rule_id = rule_id;
    state->last_value = NAN;
    return state;
}

static bool check_condition(db_alarm_rule_t *rule, float value, float hysteresis) {
    switch (rule->condition) {
        case ALARM_CONDITION_ABOVE_THRESHOLD:
            return value > (rule->threshold_high - hysteresis);
        case ALARM_CONDITION_BELOW_THRESHOLD:
            return value < (rule->threshold_low + hysteresis);
        case ALARM_CONDITION_OUT_OF_RANGE:
            return value > (rule->threshold_high - hysteresis) || 
                   value < (rule->threshold_low + hysteresis);
        default:
            return false;
    }
}

static bool check_clear_condition(db_alarm_rule_t *rule, float value, float hysteresis) {
    switch (rule->condition) {
        case ALARM_CONDITION_ABOVE_THRESHOLD:
            return value < (rule->threshold_high - hysteresis);
        case ALARM_CONDITION_BELOW_THRESHOLD:
            return value > (rule->threshold_low + hysteresis);
        case ALARM_CONDITION_OUT_OF_RANGE:
            return value < (rule->threshold_high - hysteresis) && 
                   value > (rule->threshold_low + hysteresis);
        default:
            return true;
    }
}

static void raise_alarm(db_alarm_rule_t *rule, alarm_rule_state_t *state, float value) {
    db_alarm_history_t alarm = {0};
    alarm.rule_id = rule->id;
    alarm.module_id = rule->module_id;
    alarm.severity = rule->severity;
    alarm.trigger_value = value;
    
    switch (rule->condition) {
        case ALARM_CONDITION_ABOVE_THRESHOLD:
            snprintf(alarm.message, sizeof(alarm.message), 
                     "%s: %.2f exceeds %.2f", rule->name, value, rule->threshold_high);
            break;
        case ALARM_CONDITION_BELOW_THRESHOLD:
            snprintf(alarm.message, sizeof(alarm.message), 
                     "%s: %.2f below %.2f", rule->name, value, rule->threshold_low);
            break;
        case ALARM_CONDITION_OUT_OF_RANGE:
            snprintf(alarm.message, sizeof(alarm.message), 
                     "%s: %.2f out of range [%.2f, %.2f]", rule->name, value, rule->threshold_low, rule->threshold_high);
            break;
        default:
            snprintf(alarm.message, sizeof(alarm.message), "%s: Alarm triggered", rule->name);
    }
    
    int alarm_id;
    if (db_alarm_raise(g_alarm_mgr.db, &alarm, &alarm_id) == RESULT_OK) {
        state->in_alarm = true;
        state->active_alarm_id = alarm_id;
        db_event_insert(g_alarm_mgr.db, "alarm", "warning", alarm.message);

        /* Execute safety interlock if configured */
        if (rule->interlock_enabled && rule->interlock_slot > 0) {
            actuator_state_t act_state;
            uint8_t pwm_duty = rule->interlock_pwm_duty;

            switch (rule->interlock_action) {
                case INTERLOCK_ACTION_OFF:
                    act_state = ACTUATOR_STATE_OFF;
                    pwm_duty = 0;
                    break;
                case INTERLOCK_ACTION_ON:
                    act_state = ACTUATOR_STATE_ON;
                    pwm_duty = 100;
                    break;
                case INTERLOCK_ACTION_PWM:
                    act_state = ACTUATOR_STATE_ON;
                    /* pwm_duty already set from rule */
                    break;
                default:
                    act_state = ACTUATOR_STATE_OFF;
                    pwm_duty = 0;
            }

            if (actuator_manager_manual_set(&g_actuator_mgr, rule->interlock_slot,
                                            act_state, pwm_duty) == RESULT_OK) {
                LOG_WARNING("INTERLOCK: Alarm '%s' forcing slot %d to %s (safety override)",
                           rule->name, rule->interlock_slot,
                           act_state == ACTUATOR_STATE_OFF ? "OFF" : "ON");

                char interlock_msg[256];
                snprintf(interlock_msg, sizeof(interlock_msg),
                        "Safety interlock: Slot %d forced %s by alarm '%s'",
                        rule->interlock_slot,
                        act_state == ACTUATOR_STATE_OFF ? "OFF" : "ON",
                        rule->name);
                db_event_insert(g_alarm_mgr.db, "interlock", "critical", interlock_msg);
            }
        }

        if (g_alarm_mgr.on_alarm_raised) {
            g_alarm_mgr.on_alarm_raised(&alarm, g_alarm_mgr.callback_ctx);
        }
    }
}

static void clear_alarm(db_alarm_rule_t *rule, alarm_rule_state_t *state) {
    if (state->active_alarm_id > 0) {
        db_alarm_clear(g_alarm_mgr.db, state->active_alarm_id);

        char msg[256];
        snprintf(msg, sizeof(msg), "%s: Alarm cleared", rule->name);
        db_event_insert(g_alarm_mgr.db, "alarm", "info", msg);

        /* Release safety interlock if configured */
        if (rule->interlock_enabled && rule->interlock_slot > 0 && rule->release_on_clear) {
            /* Release actuator back to controller control by setting to OFF
             * (controller can then re-command if needed) */
            if (actuator_manager_manual_set(&g_actuator_mgr, rule->interlock_slot,
                                            ACTUATOR_STATE_OFF, 0) == RESULT_OK) {
                LOG_INFO("INTERLOCK: Alarm '%s' cleared, releasing slot %d back to controller",
                        rule->name, rule->interlock_slot);

                char release_msg[256];
                snprintf(release_msg, sizeof(release_msg),
                        "Safety interlock released: Slot %d returned to controller (alarm '%s' cleared)",
                        rule->interlock_slot, rule->name);
                db_event_insert(g_alarm_mgr.db, "interlock", "info", release_msg);
            }
        }

        if (g_alarm_mgr.on_alarm_cleared) {
            db_alarm_history_t alarm = {0};
            alarm.id = state->active_alarm_id;
            alarm.rule_id = rule->id;
            alarm.module_id = rule->module_id;
            alarm.state = ALARM_STATE_CLEARED;
            g_alarm_mgr.on_alarm_cleared(&alarm, g_alarm_mgr.callback_ctx);
        }
    }

    state->in_alarm = false;
    state->active_alarm_id = 0;
}

static void check_rule(db_alarm_rule_t *rule, float current_value) {
    if (!rule->enabled) return;
    
    alarm_rule_state_t *state = get_or_create_state(rule->id);
    if (!state) return;
    
    float range = fabsf(rule->threshold_high - rule->threshold_low);
    float hysteresis = range * rule->hysteresis_percent / 100.0f;
    
    if (state->in_alarm) {
        if (rule->auto_clear && check_clear_condition(rule, current_value, hysteresis)) {
            clear_alarm(rule, state);
        }
    } else {
        if (check_condition(rule, current_value, 0)) {
            raise_alarm(rule, state, current_value);
        }
    }
    
    state->last_value = current_value;
    state->last_check_time = get_time_ms();
}

static void* alarm_check_thread(void *arg) {
    UNUSED(arg);
    
    while (g_alarm_mgr.running) {
        pthread_mutex_lock(&g_alarm_mgr.mutex);
        
        db_alarm_rule_t *rules = NULL;
        int count = 0;
        
        if (db_alarm_rule_list(g_alarm_mgr.db, &rules, &count) == RESULT_OK && rules) {
            for (int i = 0; i < count; i++) {
                if (!rules[i].enabled) continue;
                
                float value;
                char status[16];
                if (db_sensor_status_get(g_alarm_mgr.db, rules[i].module_id, &value, status, sizeof(status)) == RESULT_OK) {
                    if (strcmp(status, "ok") == 0 || strcmp(status, "unknown") == 0) {
                        check_rule(&rules[i], value);
                    }
                }
            }
            free(rules);
        }
        
        pthread_mutex_unlock(&g_alarm_mgr.mutex);
        usleep(ALARM_CHECK_INTERVAL_MS * 1000);
    }
    
    return NULL;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

result_t alarm_manager_init(database_t *db) {
    CHECK_NULL(db);
    if (g_alarm_mgr.initialized) return RESULT_OK;
    
    memset(&g_alarm_mgr, 0, sizeof(g_alarm_mgr));
    g_alarm_mgr.db = db;
    pthread_mutex_init(&g_alarm_mgr.mutex, NULL);
    g_alarm_mgr.initialized = true;
    
    LOG_INFO("Alarm manager initialized");
    return RESULT_OK;
}

result_t alarm_manager_start(void) {
    if (!g_alarm_mgr.initialized) return RESULT_NOT_INITIALIZED;
    if (g_alarm_mgr.running) return RESULT_OK;
    
    g_alarm_mgr.running = true;
    
    if (pthread_create(&g_alarm_mgr.check_thread, NULL, alarm_check_thread, NULL) != 0) {
        LOG_ERROR("Failed to create alarm thread");
        g_alarm_mgr.running = false;
        return RESULT_ERROR;
    }
    
    LOG_INFO("Alarm manager started");
    return RESULT_OK;
}

result_t alarm_manager_stop(void) {
    if (!g_alarm_mgr.running) return RESULT_OK;
    
    g_alarm_mgr.running = false;
    pthread_join(g_alarm_mgr.check_thread, NULL);
    
    LOG_INFO("Alarm manager stopped");
    return RESULT_OK;
}

void alarm_manager_shutdown(void) {
    alarm_manager_stop();
    pthread_mutex_destroy(&g_alarm_mgr.mutex);
    g_alarm_mgr.initialized = false;
    LOG_INFO("Alarm manager shutdown");
}

result_t alarm_manager_set_callbacks(alarm_callback_t on_raised, alarm_callback_t on_cleared, void *ctx) {
    pthread_mutex_lock(&g_alarm_mgr.mutex);
    g_alarm_mgr.on_alarm_raised = on_raised;
    g_alarm_mgr.on_alarm_cleared = on_cleared;
    g_alarm_mgr.callback_ctx = ctx;
    pthread_mutex_unlock(&g_alarm_mgr.mutex);
    return RESULT_OK;
}

result_t alarm_manager_check_value(int module_id, float value) {
    if (!g_alarm_mgr.initialized) return RESULT_NOT_INITIALIZED;
    
    pthread_mutex_lock(&g_alarm_mgr.mutex);
    
    db_alarm_rule_t *rules = NULL;
    int count = 0;
    
    if (db_alarm_rule_list_by_module(g_alarm_mgr.db, module_id, &rules, &count) == RESULT_OK && rules) {
        for (int i = 0; i < count; i++) check_rule(&rules[i], value);
        free(rules);
    }
    
    pthread_mutex_unlock(&g_alarm_mgr.mutex);
    return RESULT_OK;
}

result_t alarm_manager_acknowledge(int alarm_id, const char *user) {
    if (!g_alarm_mgr.initialized) return RESULT_NOT_INITIALIZED;
    return db_alarm_acknowledge(g_alarm_mgr.db, alarm_id, user);
}

result_t alarm_manager_acknowledge_all(const char *user) {
    if (!g_alarm_mgr.initialized) return RESULT_NOT_INITIALIZED;
    
    db_alarm_history_t *alarms = NULL;
    int count = 0;
    
    if (db_alarm_list_active(g_alarm_mgr.db, &alarms, &count) == RESULT_OK && alarms) {
        for (int i = 0; i < count; i++) {
            if (alarms[i].state == ALARM_STATE_ACTIVE)
                db_alarm_acknowledge(g_alarm_mgr.db, alarms[i].id, user);
        }
        free(alarms);
    }
    return RESULT_OK;
}

result_t alarm_manager_get_active_count(int *count) {
    CHECK_NULL(count);
    if (!g_alarm_mgr.initialized) return RESULT_NOT_INITIALIZED;
    return db_alarm_count_active(g_alarm_mgr.db, count);
}

result_t alarm_manager_get_active_by_severity(alarm_severity_t severity, int *count) {
    CHECK_NULL(count);
    if (!g_alarm_mgr.initialized) return RESULT_NOT_INITIALIZED;
    return db_alarm_count_by_severity(g_alarm_mgr.db, severity, count);
}

result_t alarm_manager_create_rule(int module_id, const char *name, alarm_condition_t condition,
                                   float threshold_high, float threshold_low, 
                                   alarm_severity_t severity, int *rule_id) {
    CHECK_NULL(name); CHECK_NULL(rule_id);
    if (!g_alarm_mgr.initialized) return RESULT_NOT_INITIALIZED;
    
    db_alarm_rule_t rule = {0};
    rule.module_id = module_id;
    SAFE_STRNCPY(rule.name, name, sizeof(rule.name));
    rule.condition = condition;
    rule.threshold_high = threshold_high;
    rule.threshold_low = threshold_low;
    rule.severity = severity;
    rule.enabled = true;
    rule.auto_clear = true;
    rule.hysteresis_percent = 5;
    
    return db_alarm_rule_create(g_alarm_mgr.db, &rule, rule_id);
}

result_t alarm_manager_delete_rule(int rule_id) {
    if (!g_alarm_mgr.initialized) return RESULT_NOT_INITIALIZED;
    
    pthread_mutex_lock(&g_alarm_mgr.mutex);
    db_alarm_clear_by_rule(g_alarm_mgr.db, rule_id);
    
    for (int i = 0; i < g_alarm_mgr.state_count; i++) {
        if (g_alarm_mgr.states[i].rule_id == rule_id) {
            memmove(&g_alarm_mgr.states[i], &g_alarm_mgr.states[i + 1], 
                    (g_alarm_mgr.state_count - i - 1) * sizeof(alarm_rule_state_t));
            g_alarm_mgr.state_count--;
            break;
        }
    }
    
    result_t r = db_alarm_rule_delete(g_alarm_mgr.db, rule_id);
    pthread_mutex_unlock(&g_alarm_mgr.mutex);
    return r;
}

result_t alarm_manager_enable_rule(int rule_id, bool enabled) {
    if (!g_alarm_mgr.initialized) return RESULT_NOT_INITIALIZED;
    
    pthread_mutex_lock(&g_alarm_mgr.mutex);
    
    if (!enabled) {
        alarm_rule_state_t *state = find_state(rule_id);
        if (state && state->in_alarm) {
            db_alarm_rule_t rule;
            if (db_alarm_rule_get(g_alarm_mgr.db, rule_id, &rule) == RESULT_OK)
                clear_alarm(&rule, state);
        }
    }
    
    result_t r = db_alarm_rule_set_enabled(g_alarm_mgr.db, rule_id, enabled);
    pthread_mutex_unlock(&g_alarm_mgr.mutex);
    return r;
}

bool alarm_manager_is_running(void) {
    return g_alarm_mgr.running;
}
