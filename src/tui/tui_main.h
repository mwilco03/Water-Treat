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

#endif
