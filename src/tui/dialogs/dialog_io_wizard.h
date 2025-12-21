/**
 * @file dialog_io_wizard.h
 * @brief Progressive Disclosure I/O Configuration Wizard
 *
 * Implements the core architecture principle: "User's mental model is
 * 'I plugged a wire into pin X, help me tell the system what it is'"
 *
 * Design Philosophy:
 * - Dynamic Discovery Over Static Configuration
 * - Reasonable Assumptions (system infers technical details)
 * - Graceful Degradation (conflicts shown, not blocked)
 * - Single Source of Truth (user points, system derives)
 * - Informational Output (show what was discovered)
 *
 * Navigation Contract:
 * - ESC always goes back exactly one step. Always. No exceptions.
 *
 * Screen Flow:
 * 1. INPUT or OUTPUT?
 * 2. Connection type (Scan/GPIO/ADC for inputs, Relay/PWM for outputs)
 * 3. Device/pin selection (discovery-driven)
 * 4. Name it
 * 5. Confirm with smart defaults
 */

#ifndef DIALOG_IO_WIZARD_H
#define DIALOG_IO_WIZARD_H

#include "common.h"
#include <ncurses.h>
#include <stdbool.h>

/* ============================================================================
 * Wizard Result Types
 * ========================================================================== */

/**
 * What type of I/O was created
 */
typedef enum {
    IO_WIZARD_RESULT_CANCELLED = 0,  /* User cancelled */
    IO_WIZARD_RESULT_SENSOR,         /* Created a sensor (input) */
    IO_WIZARD_RESULT_ACTUATOR,       /* Created an actuator (output) */
} io_wizard_result_type_t;

/**
 * Result of running the wizard
 */
typedef struct {
    io_wizard_result_type_t type;
    int created_id;                  /* Database ID of created entity */
    int assigned_slot;               /* PROFINET slot assigned */
    char name[64];                   /* Name given by user */
} io_wizard_result_t;

/* ============================================================================
 * Public API
 * ========================================================================== */

/**
 * Run the I/O configuration wizard
 *
 * This is the main entry point. The wizard guides the user through
 * progressive disclosure screens to configure a new sensor or actuator.
 *
 * The wizard handles:
 * - Hardware discovery (I2C, 1-Wire scanning)
 * - GPIO pin selection with conflict detection
 * - ADC channel selection for analog sensors
 * - Automatic PROFINET slot assignment
 * - Smart defaults based on device type
 *
 * @param result Output: what was created (if anything)
 * @return true if something was created, false if cancelled
 */
bool dialog_io_wizard_run(io_wizard_result_t *result);

/**
 * Run wizard starting at sensor (input) flow
 * Skips the initial INPUT/OUTPUT selection screen.
 *
 * @param result Output: what was created
 * @return true if sensor was created, false if cancelled
 */
bool dialog_io_wizard_add_sensor(io_wizard_result_t *result);

/**
 * Run wizard starting at actuator (output) flow
 * Skips the initial INPUT/OUTPUT selection screen.
 *
 * @param result Output: what was created
 * @return true if actuator was created, false if cancelled
 */
bool dialog_io_wizard_add_actuator(io_wizard_result_t *result);

#endif /* DIALOG_IO_WIZARD_H */
