#ifndef DB_ALARMS_H
#define DB_ALARMS_H

#include "common.h"
#include "database.h"

typedef enum {
    ALARM_SEVERITY_LOW = 0,
    ALARM_SEVERITY_MEDIUM,
    ALARM_SEVERITY_HIGH,
    ALARM_SEVERITY_CRITICAL
} alarm_severity_t;

typedef enum {
    ALARM_CONDITION_ABOVE_THRESHOLD = 0,
    ALARM_CONDITION_BELOW_THRESHOLD,
    ALARM_CONDITION_OUT_OF_RANGE,
    ALARM_CONDITION_RATE_OF_CHANGE,
    ALARM_CONDITION_DEVIATION
} alarm_condition_t;

typedef enum {
    ALARM_STATE_ACTIVE = 0,
    ALARM_STATE_ACKNOWLEDGED,
    ALARM_STATE_CLEARED
} alarm_state_t;

/**
 * Interlock action when alarm triggers
 */
typedef enum {
    INTERLOCK_ACTION_NONE = 0,   /* No actuator action */
    INTERLOCK_ACTION_OFF,        /* Turn actuator OFF */
    INTERLOCK_ACTION_ON,         /* Turn actuator ON */
    INTERLOCK_ACTION_PWM,        /* Set PWM duty cycle */
} interlock_action_t;

typedef struct {
    int id;
    int module_id;
    char name[MAX_NAME_LEN];
    alarm_condition_t condition;
    float threshold_high;
    float threshold_low;
    alarm_severity_t severity;
    bool enabled;
    bool auto_clear;
    int hysteresis_percent;

    /* Safety Interlock - actuator override when alarm triggers */
    bool interlock_enabled;           /* Enable actuator interlock */
    int interlock_slot;               /* Target actuator slot (9-16) */
    interlock_action_t interlock_action;  /* Action to take on alarm */
    uint8_t interlock_pwm_duty;       /* PWM duty if action=PWM (0-100) */
    bool release_on_clear;            /* Release to controller when alarm clears */
} db_alarm_rule_t;

typedef struct {
    int id;
    int rule_id;
    int module_id;
    alarm_severity_t severity;
    alarm_state_t state;
    float trigger_value;
    char message[256];
    time_t raised_time;
    time_t acknowledged_time;
    time_t cleared_time;
    char acknowledged_by[64];
} db_alarm_history_t;

// Alarm rule operations
result_t db_alarm_rule_create(database_t *db, db_alarm_rule_t *rule, int *rule_id);
result_t db_alarm_rule_update(database_t *db, db_alarm_rule_t *rule);
result_t db_alarm_rule_delete(database_t *db, int rule_id);
result_t db_alarm_rule_get(database_t *db, int rule_id, db_alarm_rule_t *rule);
result_t db_alarm_rule_list(database_t *db, db_alarm_rule_t **rules, int *count);
result_t db_alarm_rule_list_by_module(database_t *db, int module_id, db_alarm_rule_t **rules, int *count);
result_t db_alarm_rule_set_enabled(database_t *db, int rule_id, bool enabled);

// Alarm history operations
result_t db_alarm_raise(database_t *db, db_alarm_history_t *alarm, int *alarm_id);
result_t db_alarm_get(database_t *db, int alarm_id, db_alarm_history_t *alarm);
result_t db_alarm_acknowledge(database_t *db, int alarm_id, const char *user);
result_t db_alarm_clear(database_t *db, int alarm_id);
result_t db_alarm_clear_by_rule(database_t *db, int rule_id);
result_t db_alarm_list_active(database_t *db, db_alarm_history_t **alarms, int *count);
result_t db_alarm_list_history(database_t *db, int limit, db_alarm_history_t **alarms, int *count);
result_t db_alarm_count_active(database_t *db, int *count);
result_t db_alarm_count_by_severity(database_t *db, alarm_severity_t severity, int *count);

// Utility functions
const char* alarm_severity_to_string(alarm_severity_t severity);
const char* alarm_condition_to_string(alarm_condition_t condition);
const char* alarm_state_to_string(alarm_state_t state);

// Utility
void db_alarm_rule_free_list(db_alarm_rule_t *rules);
void db_alarm_history_free_list(db_alarm_history_t *alarms);

#endif
