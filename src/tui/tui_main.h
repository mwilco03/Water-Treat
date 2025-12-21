#ifndef TUI_MAIN_H
#define TUI_MAIN_H

#include "common.h"
#include "db/database.h"
#include "config/config.h"

result_t tui_init(database_t *db, config_manager_t *config, app_config_t *app_config);
void tui_run(void);
void tui_shutdown(void);

void tui_set_status(const char *fmt, ...);
void tui_request_redraw(void);
void tui_quit(void);
bool tui_is_running(void);

/**
 * @brief Check if TUI is currently active and rendering
 *
 * This function is used by the logger to determine whether to write
 * log messages to the console or route them through the TUI message area.
 * When TUI is active, direct console writes would corrupt the display.
 *
 * @return true if TUI is initialized and running, false otherwise
 */
bool tui_is_active(void);

/**
 * @brief Log a message to the TUI message area
 *
 * Routes log messages through the TUI instead of directly to console.
 * Messages appear in the status bar area and are queued in a ring buffer.
 *
 * @param level Log level (LOG_LEVEL_INFO, LOG_LEVEL_WARNING, etc.)
 * @param message The formatted message to display
 */
void tui_log_message(int level, const char *message);

#endif
