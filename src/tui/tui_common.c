/**
 * @file tui_common.c
 * @brief Common TUI utilities and shared context
 */

#include "tui_common.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>
#include <ctype.h>

/* Shared context for all TUI pages */
static struct {
    database_t *db;
    config_manager_t *config;
    app_config_t *app_config;
} g_ctx = {0};

/* ============================================================================
 * Context Management
 * ========================================================================== */

void tui_set_context(database_t *db, config_manager_t *config, app_config_t *app_config) {
    g_ctx.db = db;
    g_ctx.config = config;
    g_ctx.app_config = app_config;
}

database_t* tui_get_database(void) {
    return g_ctx.db;
}

config_manager_t* tui_get_config_manager(void) {
    return g_ctx.config;
}

app_config_t* tui_get_app_config(void) {
    return g_ctx.app_config;
}

/* ============================================================================
 * Drawing Utilities
 * ========================================================================== */

void tui_draw_box(WINDOW *win, int y, int x, int height, int width, const char *title) {
    // Draw border
    mvwhline(win, y, x, ACS_HLINE, width);
    mvwhline(win, y + height - 1, x, ACS_HLINE, width);
    mvwvline(win, y, x, ACS_VLINE, height);
    mvwvline(win, y, x + width - 1, ACS_VLINE, height);
    
    // Corners
    mvwaddch(win, y, x, ACS_ULCORNER);
    mvwaddch(win, y, x + width - 1, ACS_URCORNER);
    mvwaddch(win, y + height - 1, x, ACS_LLCORNER);
    mvwaddch(win, y + height - 1, x + width - 1, ACS_LRCORNER);
    
    // Title
    if (title && strlen(title) > 0) {
        int title_len = strlen(title) + 2;
        int title_x = x + (width - title_len) / 2;
        wattron(win, A_BOLD);
        mvwprintw(win, y, title_x, " %s ", title);
        wattroff(win, A_BOLD);
    }
}

void tui_draw_hline(WINDOW *win, int y, int x, int width) {
    mvwhline(win, y, x, ACS_HLINE, width);
}

void tui_draw_vline(WINDOW *win, int y, int x, int height) {
    mvwvline(win, y, x, ACS_VLINE, height);
}

void tui_draw_progress_bar(WINDOW *win, int y, int x, int width, float percent, int color) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    int filled = (int)((width - 2) * percent / 100.0f);
    
    mvwaddch(win, y, x, '[');
    
    wattron(win, COLOR_PAIR(color));
    for (int i = 0; i < width - 2; i++) {
        if (i < filled) {
            waddch(win, ACS_CKBOARD);
        } else {
            waddch(win, ' ');
        }
    }
    wattroff(win, COLOR_PAIR(color));
    
    waddch(win, ']');
}

void tui_draw_status_indicator(WINDOW *win, int y, int x, const char *label, bool status) {
    mvwprintw(win, y, x, "%s: ", label);
    
    if (status) {
        wattron(win, COLOR_PAIR(TUI_COLOR_STATUS) | A_BOLD);
        wprintw(win, "OK");
        wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS) | A_BOLD);
    } else {
        wattron(win, COLOR_PAIR(TUI_COLOR_ERROR) | A_BOLD);
        wprintw(win, "FAIL");
        wattroff(win, COLOR_PAIR(TUI_COLOR_ERROR) | A_BOLD);
    }
}

void tui_draw_label_value(WINDOW *win, int y, int x, const char *label, const char *value, int color) {
    mvwprintw(win, y, x, "%s: ", label);
    wattron(win, COLOR_PAIR(color));
    wprintw(win, "%s", value);
    wattroff(win, COLOR_PAIR(color));
}

void tui_draw_label_float(WINDOW *win, int y, int x, const char *label, float value, 
                          int decimals, const char *unit, int color) {
    mvwprintw(win, y, x, "%s: ", label);
    wattron(win, COLOR_PAIR(color));
    wprintw(win, "%.*f", decimals, value);
    if (unit && strlen(unit) > 0) {
        wprintw(win, " %s", unit);
    }
    wattroff(win, COLOR_PAIR(color));
}

void tui_draw_label_int(WINDOW *win, int y, int x, const char *label, int value, int color) {
    mvwprintw(win, y, x, "%s: ", label);
    wattron(win, COLOR_PAIR(color));
    wprintw(win, "%d", value);
    wattroff(win, COLOR_PAIR(color));
}

/* ============================================================================
 * Input Utilities
 * ========================================================================== */

int tui_get_string(WINDOW *win, int y, int x, char *buffer, int max_len, const char *initial) {
    if (initial) {
        strncpy(buffer, initial, max_len - 1);
        buffer[max_len - 1] = '\0';
    } else {
        buffer[0] = '\0';
    }
    
    int pos = strlen(buffer);
    int start_x = x;
    
    curs_set(1);
    keypad(win, TRUE);
    
    while (1) {
        // Clear and redraw
        mvwhline(win, y, start_x, ' ', max_len + 1);
        mvwprintw(win, y, start_x, "%s", buffer);
        wmove(win, y, start_x + pos);
        wrefresh(win);
        
        int ch = wgetch(win);
        
        switch (ch) {
            case '\n':
            case KEY_ENTER:
                curs_set(0);
                return 1;  // Accepted
                
            case 27:  // Escape
                curs_set(0);
                return 0;  // Cancelled
                
            case KEY_BACKSPACE:
            case 127:
                if (pos > 0) {
                    memmove(&buffer[pos - 1], &buffer[pos], strlen(buffer) - pos + 1);
                    pos--;
                }
                break;
                
            case KEY_DC:  // Delete
                if (pos < (int)strlen(buffer)) {
                    memmove(&buffer[pos], &buffer[pos + 1], strlen(buffer) - pos);
                }
                break;
                
            case KEY_LEFT:
                if (pos > 0) pos--;
                break;
                
            case KEY_RIGHT:
                if (pos < (int)strlen(buffer)) pos++;
                break;
                
            case KEY_HOME:
                pos = 0;
                break;
                
            case KEY_END:
                pos = strlen(buffer);
                break;
                
            default:
                if (ch >= 32 && ch < 127 && (int)strlen(buffer) < max_len - 1) {
                    memmove(&buffer[pos + 1], &buffer[pos], strlen(buffer) - pos + 1);
                    buffer[pos++] = ch;
                }
                break;
        }
    }
}

int tui_get_int(WINDOW *win, int y, int x, int *value, int min_val, int max_val) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", *value);
    
    if (tui_get_string(win, y, x, buffer, sizeof(buffer), buffer)) {
        int new_val = atoi(buffer);
        if (new_val >= min_val && new_val <= max_val) {
            *value = new_val;
            return 1;
        }
    }
    return 0;
}

int tui_get_float(WINDOW *win, int y, int x, float *value, float min_val, float max_val) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.4f", *value);
    
    if (tui_get_string(win, y, x, buffer, sizeof(buffer), buffer)) {
        float new_val = atof(buffer);
        if (new_val >= min_val && new_val <= max_val) {
            *value = new_val;
            return 1;
        }
    }
    return 0;
}

bool tui_confirm(WINDOW *win, const char *message) {
    int max_y, max_x;
    getmaxyx(win, max_y, max_x);
    
    int width = strlen(message) + 20;
    int height = 5;
    int start_y = (max_y - height) / 2;
    int start_x = (max_x - width) / 2;
    
    WINDOW *dialog = newwin(height, width, start_y, start_x);
    box(dialog, 0, 0);
    
    mvwprintw(dialog, 2, 2, "%s (y/n)", message);
    wrefresh(dialog);
    
    int ch;
    do {
        ch = wgetch(dialog);
    } while (ch != 'y' && ch != 'Y' && ch != 'n' && ch != 'N' && ch != 27);
    
    delwin(dialog);
    
    return (ch == 'y' || ch == 'Y');
}

int tui_menu(WINDOW *win, const char *title, const char **options, int count) {
    int max_y, max_x;
    getmaxyx(win, max_y, max_x);
    
    // Calculate dimensions
    int max_opt_len = strlen(title);
    for (int i = 0; i < count; i++) {
        int len = strlen(options[i]);
        if (len > max_opt_len) max_opt_len = len;
    }
    
    int width = max_opt_len + 8;
    int height = count + 4;
    int start_y = (max_y - height) / 2;
    int start_x = (max_x - width) / 2;
    
    WINDOW *menu = newwin(height, width, start_y, start_x);
    keypad(menu, TRUE);
    
    int selected = 0;
    
    while (1) {
        werase(menu);
        box(menu, 0, 0);
        
        wattron(menu, A_BOLD);
        mvwprintw(menu, 0, (width - strlen(title) - 2) / 2, " %s ", title);
        wattroff(menu, A_BOLD);
        
        for (int i = 0; i < count; i++) {
            if (i == selected) {
                wattron(menu, A_REVERSE);
            }
            mvwprintw(menu, i + 2, 2, " %s ", options[i]);
            if (i == selected) {
                wattroff(menu, A_REVERSE);
            }
        }
        
        wrefresh(menu);
        
        int ch = wgetch(menu);
        switch (ch) {
            case KEY_UP:
                if (selected > 0) selected--;
                break;
            case KEY_DOWN:
                if (selected < count - 1) selected++;
                break;
            case '\n':
            case KEY_ENTER:
                delwin(menu);
                return selected;
            case 27:  // Escape
                delwin(menu);
                return -1;
        }
    }
}

/* ============================================================================
 * Formatting Utilities
 * ========================================================================== */

void tui_format_size(char *buffer, size_t buf_size, uint64_t bytes) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = bytes;
    
    while (size >= 1024 && unit < 4) {
        size /= 1024;
        unit++;
    }
    
    if (unit == 0) {
        snprintf(buffer, buf_size, "%lu %s", (unsigned long)bytes, units[unit]);
    } else {
        snprintf(buffer, buf_size, "%.1f %s", size, units[unit]);
    }
}

void tui_format_duration(char *buffer, size_t buf_size, int seconds) {
    int days = seconds / 86400;
    int hours = (seconds % 86400) / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;
    
    if (days > 0) {
        snprintf(buffer, buf_size, "%dd %dh %dm", days, hours, mins);
    } else if (hours > 0) {
        snprintf(buffer, buf_size, "%dh %dm %ds", hours, mins, secs);
    } else if (mins > 0) {
        snprintf(buffer, buf_size, "%dm %ds", mins, secs);
    } else {
        snprintf(buffer, buf_size, "%ds", secs);
    }
}

void tui_format_timestamp(char *buffer, size_t buf_size, time_t timestamp) {
    struct tm *tm = localtime(&timestamp);
    strftime(buffer, buf_size, "%Y-%m-%d %H:%M:%S", tm);
}

/* ============================================================================
 * Color Utilities
 * ========================================================================== */

int tui_status_color(const char *status) {
    if (!status) return TUI_COLOR_NORMAL;
    
    if (strcmp(status, "ok") == 0 || strcmp(status, "good") == 0 || 
        strcmp(status, "connected") == 0 || strcmp(status, "active") == 0) {
        return TUI_COLOR_STATUS;
    }
    
    if (strcmp(status, "error") == 0 || strcmp(status, "fail") == 0 ||
        strcmp(status, "critical") == 0 || strcmp(status, "disconnected") == 0) {
        return TUI_COLOR_ERROR;
    }
    
    if (strcmp(status, "warning") == 0 || strcmp(status, "warn") == 0) {
        return TUI_COLOR_WARNING;
    }
    
    return TUI_COLOR_NORMAL;
}

int tui_value_color(float value, float low_warn, float low_err, float high_warn, float high_err) {
    if (value <= low_err || value >= high_err) return TUI_COLOR_ERROR;
    if (value <= low_warn || value >= high_warn) return TUI_COLOR_WARNING;
    return TUI_COLOR_STATUS;
}
