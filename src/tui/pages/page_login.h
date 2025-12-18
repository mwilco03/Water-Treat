/**
 * @file page_login.h
 * @brief Login page for TUI authentication
 */

#ifndef PAGE_LOGIN_H
#define PAGE_LOGIN_H

#include "common.h"
#include <stdbool.h>

/**
 * Initialize login page
 */
void page_login_init(void);

/**
 * Draw login page
 */
void page_login_draw(void);

/**
 * Handle input on login page
 * @return true if input was handled
 */
bool page_login_input(int ch);

/**
 * Cleanup login page
 */
void page_login_cleanup(void);

/**
 * Run login screen (blocking until success or quit)
 * @return RESULT_OK on successful login, RESULT_ERROR on quit
 */
result_t page_login_run(void);

/**
 * Check if login is required
 */
bool page_login_required(void);

#endif /* PAGE_LOGIN_H */
