#ifndef TUI_COMMON_H
#define TUI_COMMON_H

#include "common.h"
#include "db/database.h"
#include "config/config.h"
#include <ncurses.h>
#include <stdarg.h>

// Color pair definitions
typedef enum {
    TUI_COLOR_NORMAL = 1,
    TUI_COLOR_HEADER,
    TUI_COLOR_TITLE,
    TUI_COLOR_STATUS,
    TUI_COLOR_ERROR,
    TUI_COLOR_WARNING,
    TUI_COLOR_HIGHLIGHT,
    TUI_COLOR_INPUT
} tui_color_t;

// Context management
void tui_set_context(database_t *db, config_manager_t *config, app_config_t *app_config);
database_t* tui_get_database(void);
config_manager_t* tui_get_config_manager(void);
app_config_t* tui_get_app_config(void);

// Drawing utilities
void tui_draw_box(WINDOW *win, int y, int x, int height, int width, const char *title);
void tui_draw_hline(WINDOW *win, int y, int x, int width);
void tui_draw_vline(WINDOW *win, int y, int x, int height);
void tui_draw_progress_bar(WINDOW *win, int y, int x, int width, float percent, int color);
void tui_draw_status_indicator(WINDOW *win, int y, int x, const char *label, bool status);
void tui_draw_label_value(WINDOW *win, int y, int x, const char *label, const char *value, int color);
void tui_draw_label_float(WINDOW *win, int y, int x, const char *label, float value, int decimals, const char *unit, int color);
void tui_draw_label_int(WINDOW *win, int y, int x, const char *label, int value, int color);

// Input utilities
int tui_get_string(WINDOW *win, int y, int x, char *buffer, int max_len, const char *initial);
int tui_get_int(WINDOW *win, int y, int x, int *value, int min_val, int max_val);
int tui_get_float(WINDOW *win, int y, int x, float *value, float min_val, float max_val);
bool tui_confirm(WINDOW *win, const char *message);
int tui_menu(WINDOW *win, const char *title, const char **options, int count);

// Formatting utilities
void tui_format_size(char *buffer, size_t buf_size, uint64_t bytes);
void tui_format_duration(char *buffer, size_t buf_size, int seconds);
void tui_format_timestamp(char *buffer, size_t buf_size, time_t timestamp);

// Color utilities
int tui_status_color(const char *status);
int tui_value_color(float value, float low_warn, float low_err, float high_warn, float high_err);

#endif
