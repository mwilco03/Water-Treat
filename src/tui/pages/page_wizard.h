/**
 * @file page_wizard.h
 * @brief First-run setup wizard
 */

#ifndef PAGE_WIZARD_H
#define PAGE_WIZARD_H

#include "tui/tui_common.h"

/**
 * @brief Check if setup wizard should be shown
 * @return true if first run detected
 */
bool wizard_should_show(void);

/**
 * @brief Initialize the setup wizard page
 */
void page_wizard_init(void);

/**
 * @brief Draw the wizard page
 */
void page_wizard_draw(void);

/**
 * @brief Handle input for wizard page
 * @param ch Input character
 * @return true if input was handled
 */
bool page_wizard_input(int ch);

/**
 * @brief Cleanup wizard page
 */
void page_wizard_cleanup(void);

/**
 * @brief Run the complete setup wizard
 * @return RESULT_OK if completed, RESULT_ERROR if cancelled
 */
result_t wizard_run(void);

#endif /* PAGE_WIZARD_H */
