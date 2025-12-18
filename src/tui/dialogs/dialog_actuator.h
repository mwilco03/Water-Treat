/**
 * @file dialog_actuator.h
 * @brief Actuator add/edit/delete dialogs
 */

#ifndef DIALOG_ACTUATOR_H
#define DIALOG_ACTUATOR_H

#include "common.h"
#include "db/db_actuators.h"
#include <stdbool.h>

/* Dialog mode */
typedef enum {
    ACTUATOR_DIALOG_ADD,
    ACTUATOR_DIALOG_EDIT,
} actuator_dialog_mode_t;

/* Actuator form data - matches db_actuator_t structure */
typedef struct {
    int id;
    char name[MAX_NAME_LEN];
    int slot;
    int subslot;
    actuator_type_t type;
    int gpio_pin;
    char gpio_chip[32];
    bool active_low;

    /* PWM settings */
    int pwm_frequency_hz;

    /* Safety settings */
    safe_state_t safe_state;
    int min_on_time_ms;
    int max_on_time_ms;

    bool enabled;
} actuator_form_t;

/**
 * Show actuator add/edit dialog
 * @param mode ACTUATOR_DIALOG_ADD or ACTUATOR_DIALOG_EDIT
 * @param form Form data (pre-filled for edit mode)
 * @return true if user confirmed, false if cancelled
 */
bool dialog_actuator_show(actuator_dialog_mode_t mode, actuator_form_t *form);

/**
 * Show delete confirmation dialog
 * @param actuator_name Name of actuator to delete
 * @return true if user confirmed deletion
 */
bool dialog_actuator_confirm_delete(const char *actuator_name);

/**
 * Initialize form with defaults
 */
void dialog_actuator_init_form(actuator_form_t *form);

/**
 * Load form from existing actuator record
 */
void dialog_actuator_load(actuator_form_t *form, const db_actuator_t *actuator);

/**
 * Save form to actuator structure
 */
void dialog_actuator_save(const actuator_form_t *form, db_actuator_t *actuator);

#endif /* DIALOG_ACTUATOR_H */
