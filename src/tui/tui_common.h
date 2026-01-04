#ifndef TUI_COMMON_H
#define TUI_COMMON_H

#include "common.h"
#include "db/database.h"
#include "config/config.h"
#include "sensors/sensor_manager.h"
#ifdef LED_SUPPORT
#include "hal/led_status.h"
#endif
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

void tui_set_status(const char *fmt, ...);

// Context management - extended with sensor and LED manager access
void tui_set_context(database_t *db, config_manager_t *config, app_config_t *app_config);
void tui_set_sensor_manager(sensor_manager_t *sensor_mgr);
#ifdef LED_SUPPORT
void tui_set_led_manager(led_status_manager_t *led_mgr);
led_status_manager_t* tui_get_led_manager(void);
#endif
database_t* tui_get_database(void);
config_manager_t* tui_get_config_manager(void);
app_config_t* tui_get_app_config(void);
sensor_manager_t* tui_get_sensor_manager(void);

/**
 * @brief Trigger sensor manager to reload sensors from database
 *
 * Call this after adding, editing, or deleting sensors in the TUI
 * to ensure the sensor manager picks up the changes immediately.
 */
void tui_reload_sensors(void);

/**
 * @brief Notify that a sensor configuration changed
 * @param sensor_slot Slot number of affected sensor (-1 for all)
 *
 * Updates LED status and triggers any necessary refreshes.
 */
void tui_notify_sensor_changed(int sensor_slot);

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

/* ============================================================================
 * Reusable List Widget
 * ============================================================================
 * Provides common list navigation state and input handling.
 * Use this in TUI pages to avoid duplicating scroll/selection logic.
 */

/**
 * List navigation state - embed this in your page state struct
 */
typedef struct {
    int selected;       /* Currently selected item index */
    int scroll_offset;  /* First visible item index */
    int item_count;     /* Total number of items */
    int visible_rows;   /* Number of visible rows in the list area */
} tui_list_state_t;

/**
 * Initialize list state
 */
static inline void tui_list_init(tui_list_state_t *list, int visible_rows) {
    list->selected = 0;
    list->scroll_offset = 0;
    list->item_count = 0;
    list->visible_rows = visible_rows;
}

/**
 * Update item count (call after loading/refreshing data)
 */
static inline void tui_list_set_count(tui_list_state_t *list, int count) {
    list->item_count = count;
    /* Adjust selection if out of bounds */
    if (list->selected >= count && count > 0) {
        list->selected = count - 1;
    }
    /* Adjust scroll if out of bounds */
    int max_offset = count > list->visible_rows ? count - list->visible_rows : 0;
    if (list->scroll_offset > max_offset) {
        list->scroll_offset = max_offset;
    }
}

/**
 * Handle navigation key input
 * @return true if the key was handled, false otherwise
 */
static inline bool tui_list_input(tui_list_state_t *list, int ch) {
    if (list->item_count == 0) return false;

    switch (ch) {
        case KEY_UP:
            if (list->selected > 0) {
                list->selected--;
                if (list->selected < list->scroll_offset) {
                    list->scroll_offset = list->selected;
                }
            }
            return true;

        case KEY_DOWN:
            if (list->selected < list->item_count - 1) {
                list->selected++;
                if (list->selected >= list->scroll_offset + list->visible_rows) {
                    list->scroll_offset = list->selected - list->visible_rows + 1;
                }
            }
            return true;

        case KEY_PPAGE:
            list->selected = MAX(0, list->selected - list->visible_rows);
            list->scroll_offset = MAX(0, list->scroll_offset - list->visible_rows);
            return true;

        case KEY_NPAGE: {
            int max_offset = list->item_count > list->visible_rows ?
                             list->item_count - list->visible_rows : 0;
            list->selected = MIN(list->item_count - 1,
                                 list->selected + list->visible_rows);
            list->scroll_offset = MIN(max_offset,
                                      list->scroll_offset + list->visible_rows);
            return true;
        }

        case KEY_HOME:
            list->selected = 0;
            list->scroll_offset = 0;
            return true;

        case KEY_END:
            list->selected = list->item_count > 0 ? list->item_count - 1 : 0;
            list->scroll_offset = list->item_count > list->visible_rows ?
                                  list->item_count - list->visible_rows : 0;
            return true;
    }
    return false;
}

/**
 * Get scroll percentage for display (0-100)
 */
static inline int tui_list_scroll_percent(const tui_list_state_t *list) {
    if (list->item_count <= list->visible_rows) return 0;
    int max_offset = list->item_count - list->visible_rows;
    return (list->scroll_offset * 100) / max_offset;
}

/**
 * Get number of visible items to draw
 */
static inline int tui_list_visible_count(const tui_list_state_t *list) {
    int remaining = list->item_count - list->scroll_offset;
    return MIN(list->visible_rows, remaining);
}

#endif
