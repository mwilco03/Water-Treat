/**
 * @file page_logging.c
 * @brief Logging configuration and event viewer page
 */

#include "page_logging.h"
#include "../tui_common.h"
#include "db/database.h"
#include "db/db_events.h"
#include "logging/data_logger.h"
#include "config/config.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>
#include <time.h>

#define MAX_EVENTS 100
#define VISIBLE_ROWS 10
#define MAX_FIELDS 8

typedef struct {
    int id;
    time_t timestamp;
    char source[32];
    char level[16];
    char message[256];
} event_display_t;

typedef struct {
    const char *label;
    char value[64];
    bool editable;
    const char *config_key;
} field_t;

static struct {
    WINDOW *win;
    
    // Config fields
    field_t fields[MAX_FIELDS];
    int field_count;
    int selected_field;
    
    // Event log
    event_display_t events[MAX_EVENTS];
    int event_count;
    int selected_event;
    int scroll_offset;
    
    // Stats
    data_logger_stats_t stats;
    
    bool editing;
    char edit_buffer[64];
    int edit_pos;
    
    int view_mode;  // 0 = config, 1 = events
} g_page = {0};

static void load_logging_config(void) {
    g_page.field_count = 0;
    
    config_manager_t *cfg_mgr = tui_get_config_manager();
    
    field_t *f;
    char value[64];
    
    // Enabled
    f = &g_page.fields[g_page.field_count++];
    f->label = "Logging Enabled";
    f->editable = true;
    f->config_key = "enabled";
    if (cfg_mgr && config_get_string(cfg_mgr, "logging", "enabled", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "true");
    }
    
    // Interval
    f = &g_page.fields[g_page.field_count++];
    f->label = "Interval (sec)";
    f->editable = true;
    f->config_key = "interval_seconds";
    if (cfg_mgr && config_get_string(cfg_mgr, "logging", "interval_seconds", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "60");
    }
    
    // Retention
    f = &g_page.fields[g_page.field_count++];
    f->label = "Retention (days)";
    f->editable = true;
    f->config_key = "retention_days";
    if (cfg_mgr && config_get_string(cfg_mgr, "logging", "retention_days", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "30");
    }
    
    // Remote Enabled
    f = &g_page.fields[g_page.field_count++];
    f->label = "Remote Enabled";
    f->editable = true;
    f->config_key = "remote_enabled";
    if (cfg_mgr && config_get_string(cfg_mgr, "logging", "remote_enabled", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "false");
    }
    
    // Remote URL
    f = &g_page.fields[g_page.field_count++];
    f->label = "Remote URL";
    f->editable = true;
    f->config_key = "remote_url";
    if (cfg_mgr && config_get_string(cfg_mgr, "logging", "remote_url", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "");
    }
    
    // Log Level
    f = &g_page.fields[g_page.field_count++];
    f->label = "Log Level";
    f->editable = true;
    f->config_key = "log_level";
    app_config_t *app_cfg = tui_get_app_config();
    if (app_cfg) {
        SAFE_STRNCPY(f->value, app_cfg->system.log_level, sizeof(f->value));
    } else {
        strcpy(f->value, "info");
    }
}

static void load_events(void) {
    g_page.event_count = 0;
    
    database_t *db = tui_get_database();
    if (!db) return;
    
    db_event_t *events = NULL;
    int count = 0;
    
    if (db_event_list(db, MAX_EVENTS, &events, &count) != RESULT_OK || !events) return;
    
    for (int i = 0; i < count && i < MAX_EVENTS; i++) {
        event_display_t *e = &g_page.events[g_page.event_count];
        e->id = events[i].id;
        e->timestamp = events[i].timestamp;
        SAFE_STRNCPY(e->source, events[i].source, sizeof(e->source));
        SAFE_STRNCPY(e->level, events[i].level, sizeof(e->level));
        SAFE_STRNCPY(e->message, events[i].message, sizeof(e->message));
        g_page.event_count++;
    }
    
    db_event_free_list(events);
}

static void load_stats(void) {
    data_logger_get_stats(&g_page.stats);
}

static void save_field(int idx) {
    if (idx < 0 || idx >= g_page.field_count) return;
    
    field_t *f = &g_page.fields[idx];
    if (!f->editable) return;
    
    config_manager_t *cfg_mgr = tui_get_config_manager();
    if (cfg_mgr) {
        if (strcmp(f->config_key, "log_level") == 0) {
            config_set_string(cfg_mgr, "system", f->config_key, f->value);
            logger_set_level(log_level_from_string(f->value));
        } else {
            config_set_string(cfg_mgr, "logging", f->config_key, f->value);
        }
        tui_set_status("Saved: %s", f->label);
    }
}

static int level_to_color(const char *level) {
    if (strcmp(level, "error") == 0) return TUI_COLOR_ERROR;
    if (strcmp(level, "warning") == 0) return TUI_COLOR_WARNING;
    if (strcmp(level, "info") == 0) return TUI_COLOR_STATUS;
    return TUI_COLOR_NORMAL;
}

static void draw_config(WINDOW *win, int *row) {
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, *row, 2, "Logging Configuration");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    (*row)++;
    
    mvwhline(win, *row, 2, ACS_HLINE, 45);
    (*row)++;
    
    for (int i = 0; i < g_page.field_count; i++) {
        field_t *f = &g_page.fields[i];
        
        if (i == g_page.selected_field && g_page.view_mode == 0) {
            wattron(win, A_REVERSE);
        }
        
        mvwprintw(win, *row, 4, "%-16s: ", f->label);
        
        if (g_page.editing && i == g_page.selected_field) {
            wattron(win, COLOR_PAIR(TUI_COLOR_INPUT));
            wprintw(win, "%-24s", g_page.edit_buffer);
            wattroff(win, COLOR_PAIR(TUI_COLOR_INPUT));
        } else {
            wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
            wprintw(win, "%-24s", f->value);
            wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
        }
        
        if (i == g_page.selected_field && g_page.view_mode == 0) {
            wattroff(win, A_REVERSE);
        }
        
        (*row)++;
    }
    
    (*row)++;
}

static void draw_stats(WINDOW *win, int *row) {
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, *row, 50, "Logger Statistics");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    (*row)++;
    
    int stat_row = *row - 1;
    mvwhline(win, stat_row++, 50, ACS_HLINE, 30);
    
    mvwprintw(win, stat_row++, 52, "Running: %s", 
              data_logger_is_running() ? "Yes" : "No");
    mvwprintw(win, stat_row++, 52, "Logged: %lu", g_page.stats.total_logged);
    mvwprintw(win, stat_row++, 52, "Remote Sent: %lu", g_page.stats.total_remote_sent);
    mvwprintw(win, stat_row++, 52, "Remote Failed: %lu", g_page.stats.total_remote_failed);
    mvwprintw(win, stat_row++, 52, "Queue: %d/%d", 
              g_page.stats.queue_count, g_page.stats.queue_capacity);
}

static void draw_events(WINDOW *win, int *row) {
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, *row, 2, "Recent Events (%d)", g_page.event_count);
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    (*row)++;
    
    mvwhline(win, *row, 2, ACS_HLINE, getmaxx(win) - 4);
    (*row)++;
    
    // Header
    wattron(win, A_BOLD);
    mvwprintw(win, *row, 4, "%-19s %-8s %-12s %-50s",
              "Time", "Level", "Source", "Message");
    wattroff(win, A_BOLD);
    (*row)++;
    
    if (g_page.event_count == 0) {
        wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
        mvwprintw(win, *row + 1, 6, "No events recorded");
        wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
        return;
    }
    
    int visible = MIN(VISIBLE_ROWS, g_page.event_count - g_page.scroll_offset);
    
    for (int i = 0; i < visible; i++) {
        int idx = g_page.scroll_offset + i;
        event_display_t *e = &g_page.events[idx];
        
        if (idx == g_page.selected_event && g_page.view_mode == 1) {
            wattron(win, A_REVERSE);
        }
        
        char time_str[20];
        struct tm *tm = localtime(&e->timestamp);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
        
        mvwprintw(win, *row, 4, "%-19s ", time_str);
        
        int color = level_to_color(e->level);
        wattron(win, COLOR_PAIR(color));
        wprintw(win, "%-8s", e->level);
        wattroff(win, COLOR_PAIR(color));
        
        wprintw(win, " %-12s ", e->source);
        
        // Truncate message
        char msg[51];
        strncpy(msg, e->message, 50);
        msg[50] = '\0';
        wprintw(win, "%-50s", msg);
        
        if (idx == g_page.selected_event && g_page.view_mode == 1) {
            wattroff(win, A_REVERSE);
        }
        
        (*row)++;
    }
}

static void draw_help(WINDOW *win) {
    int max_y = getmaxy(win);
    int row = max_y - 2;
    
    wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwprintw(win, row, 2, "Tab:Switch  Enter:Edit  r:Refresh  c:Cleanup  Ctrl+S:Save");
    wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
}

void page_logging_init(WINDOW *win) {
    g_page.win = win;
    g_page.view_mode = 0;
    g_page.selected_field = 0;
    g_page.selected_event = 0;
    g_page.scroll_offset = 0;
    g_page.editing = false;
    
    load_logging_config();
    load_events();
    load_stats();
}

void page_logging_draw(WINDOW *win) {
    int row = 2;
    
    draw_config(win, &row);
    draw_stats(win, &row);
    
    row = 12;
    draw_events(win, &row);
    draw_help(win);
}

void page_logging_input(WINDOW *win, int ch) {
    UNUSED(win);
    
    if (g_page.editing) {
        switch (ch) {
            case 27:
                g_page.editing = false;
                break;
            case '\n':
            case KEY_ENTER:
                SAFE_STRNCPY(g_page.fields[g_page.selected_field].value,
                            g_page.edit_buffer,
                            sizeof(g_page.fields[g_page.selected_field].value));
                save_field(g_page.selected_field);
                g_page.editing = false;
                break;
            case KEY_BACKSPACE:
            case 127:
                if (g_page.edit_pos > 0) {
                    g_page.edit_buffer[--g_page.edit_pos] = '\0';
                }
                break;
            default:
                if (ch >= 32 && ch < 127 && g_page.edit_pos < (int)sizeof(g_page.edit_buffer) - 1) {
                    g_page.edit_buffer[g_page.edit_pos++] = ch;
                    g_page.edit_buffer[g_page.edit_pos] = '\0';
                }
                break;
        }
        return;
    }
    
    switch (ch) {
        case '\t':
            g_page.view_mode = (g_page.view_mode + 1) % 2;
            break;
            
        case KEY_UP:
            if (g_page.view_mode == 0) {
                if (g_page.selected_field > 0) g_page.selected_field--;
            } else {
                if (g_page.selected_event > 0) {
                    g_page.selected_event--;
                    if (g_page.selected_event < g_page.scroll_offset) {
                        g_page.scroll_offset = g_page.selected_event;
                    }
                }
            }
            break;
            
        case KEY_DOWN:
            if (g_page.view_mode == 0) {
                if (g_page.selected_field < g_page.field_count - 1) g_page.selected_field++;
            } else {
                if (g_page.selected_event < g_page.event_count - 1) {
                    g_page.selected_event++;
                    if (g_page.selected_event >= g_page.scroll_offset + VISIBLE_ROWS) {
                        g_page.scroll_offset = g_page.selected_event - VISIBLE_ROWS + 1;
                    }
                }
            }
            break;
            
        case '\n':
        case KEY_ENTER:
            if (g_page.view_mode == 0 && g_page.fields[g_page.selected_field].editable) {
                g_page.editing = true;
                SAFE_STRNCPY(g_page.edit_buffer,
                            g_page.fields[g_page.selected_field].value,
                            sizeof(g_page.edit_buffer));
                g_page.edit_pos = strlen(g_page.edit_buffer);
            }
            break;
            
        case 'r':
        case 'R':
            load_events();
            load_stats();
            tui_set_status("Refreshed");
            break;
            
        case 'c':
        case 'C':
            {
                database_t *db = tui_get_database();
                if (db) {
                    db_event_cleanup(db, 7);  // Keep last 7 days
                    load_events();
                    tui_set_status("Cleaned up old events");
                }
            }
            break;
            
        case 19:  // Ctrl+S
            {
                config_manager_t *cfg = tui_get_config_manager();
                if (cfg) {
                    config_save_file(cfg, NULL);
                    tui_set_status("Configuration saved");
                }
            }
            break;
    }
}

void page_logging_cleanup(void) {
    g_page.win = NULL;
}
