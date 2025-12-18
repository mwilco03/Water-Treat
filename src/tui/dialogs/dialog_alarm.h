/**
 * @file dialog_alarm.h
 * @brief Alarm rule add/edit dialog
 */

#ifndef DIALOG_ALARM_H
#define DIALOG_ALARM_H

#include "common.h"
#include "db/db_alarms.h"
#include <stdbool.h>

/* Dialog mode */
typedef enum {
    ALARM_DIALOG_ADD,
    ALARM_DIALOG_EDIT,
} alarm_dialog_mode_t;

/* Dialog form data - matches db_alarm_rule_t structure */
typedef struct {
    int rule_id;                /* For edit mode */
    char name[MAX_NAME_LEN];
    int module_id;
    alarm_condition_t condition;
    float threshold_high;
    float threshold_low;
    alarm_severity_t severity;
    int hysteresis_percent;
    bool enabled;
    bool auto_clear;

    /* Interlock settings */
    bool interlock_enabled;
    int interlock_slot;
    interlock_action_t interlock_action;
    uint8_t interlock_pwm_duty;
    bool release_on_clear;
} alarm_form_t;

/**
 * Show alarm rule add/edit dialog
 * @param mode ALARM_DIALOG_ADD or ALARM_DIALOG_EDIT
 * @param form Form data (pre-filled for edit mode)
 * @return true if user confirmed, false if cancelled
 */
bool dialog_alarm_show(alarm_dialog_mode_t mode, alarm_form_t *form);

/**
 * Initialize form with defaults
 */
void dialog_alarm_init_form(alarm_form_t *form);

/**
 * Load form from existing rule
 */
void dialog_alarm_load_rule(alarm_form_t *form, const db_alarm_rule_t *rule);

/**
 * Save form to rule structure
 */
void dialog_alarm_save_to_rule(const alarm_form_t *form, db_alarm_rule_t *rule);

#endif /* DIALOG_ALARM_H */
