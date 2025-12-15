/**
 * @file page_alarms.c
 * @brief Alarm management page
 */

#include "page_alarms.h"
#include "../tui_common.h"
#include "db/database.h"
#include "db/db_alarms.h"
#include "alarms/alarm_manager.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>
#include <time.h>

#define MAX_ALARMS 100
#define VISIBLE_ROWS 12

typedef struct {
    int id;
    int rule_id;
    int module_id;
    alarm_severity_t severity;
    alarm_state_t state;
    char message[128];
    float trigger_value;
    time_t raised_time;
    char acknowledged_by[32];
} alarm_display_t;

static struct {
    WINDOW *win;
    
    alarm_display_t alarms[MAX_ALARMS];
    int alarm_count;
    int selected;
    int scroll_offset;
    
    int view_mode;  // 0 = active, 1 = history, 2 = rules
    bool show_dialog;
} g_page = {0};

static void load_active_alarms(void) {
    g_page.alarm_count = 0;
    
    database_t *db = tui_get_database();
    if (!db) return;
    
    db_alarm_history_t *alarms = NULL;
    int count = 0;
    
    if (db_alarm_list_active(db, &alarms, &count) != RESULT_OK || !alarms) return;
    
    for (int i = 0; i < count && i < MAX_ALARMS; i++) {
        alarm_display_t *a = &g_page.alarms[g_page.alarm_count];
        a->id = alarms[i].id;
        a->rule_id = alarms[i].rule_id;
        a->module_id = alarms[i].module_id;
        a->severity = alarms[i].severity;
        a->state = alarms[i].state;
        SAFE_STRNCPY(a->message, alarms[i].message, sizeof(a->message));
        a->trigger_value = alarms[i].trigger_value;
        a->raised_time = alarms[i].raised_time;
        SAFE_STRNCPY(a->acknowledged_by, alarms[i].acknowledged_by, sizeof(a->acknowledged_by));
        g_page.alarm_count++;
    }
    
    free(alarms);
}

static const char* severity_to_str(alarm_severity_t sev) {
    switch (sev) {
        case ALARM_SEVERITY_LOW: return "LOW";
        case ALARM_SEVERITY_MEDIUM: return "MED";
        case ALARM_SEVERITY_HIGH: return "HIGH";
        case ALARM_SEVERITY_CRITICAL: return "CRIT";
        default: return "???";
    }
}

static int severity_to_color(alarm_severity_t sev) {
    switch (sev) {
        case ALARM_SEVERITY_LOW: return TUI_COLOR_STATUS;
        case ALARM_SEVERITY_MEDIUM: return TUI_COLOR_WARNING;
        case ALARM_SEVERITY_HIGH: return TUI_COLOR_ERROR;
        case ALARM_SEVERITY_CRITICAL: return TUI_COLOR_ERROR;
        default: return TUI_COLOR_NORMAL;
    }
}

static const char* state_to_str(alarm_state_t state) {
    switch (state) {
        case ALARM_STATE_ACTIVE: return "ACTIVE";
        case ALARM_STATE_ACKNOWLEDGED: return "ACK";
        case ALARM_STATE_CLEARED: return "CLEARED";
        default: return "???";
    }
}

static void draw_alarm_summary(WINDOW *win, int *row) {
    database_t *db = tui_get_database();
    
    int total = 0, critical = 0, high = 0, medium = 0, low = 0;
    
    if (db) {
        db_alarm_count_active(db, &total);
        db_alarm_count_by_severity(db, ALARM_SEVERITY_CRITICAL, &critical);
        db_alarm_count_by_severity(db, ALARM_SEVERITY_HIGH, &high);
        db_alarm_count_by_severity(db, ALARM_SEVERITY_MEDIUM, &medium);
        db_alarm_count_by_severity(db, ALARM_SEVERITY_LOW, &low);
    }
    
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, *row, 2, "Alarm Summary");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    (*row)++;
    
    mvwhline(win, *row, 2, ACS_HLINE, getmaxx(win) - 4);
    (*row)++;
    
    mvwprintw(win, *row, 4, "Total Active: ");
    if (total > 0) {
        wattron(win, COLOR_PAIR(TUI_COLOR_ERROR) | A_BOLD);
        wprintw(win, "%d", total);
        wattroff(win, COLOR_PAIR(TUI_COLOR_ERROR) | A_BOLD);
    } else {
        wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
        wprintw(win, "0");
        wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
    }
    
    mvwprintw(win, *row, 25, "CRIT:");
    wattron(win, COLOR_PAIR(critical > 0 ? TUI_COLOR_ERROR : TUI_COLOR_NORMAL));
    wprintw(win, "%d", critical);
    wattroff(win, COLOR_PAIR(critical > 0 ? TUI_COLOR_ERROR : TUI_COLOR_NORMAL));
    
    mvwprintw(win, *row, 35, "HIGH:");
    wattron(win, COLOR_PAIR(high > 0 ? TUI_COLOR_ERROR : TUI_COLOR_NORMAL));
    wprintw(win, "%d", high);
    wattroff(win, COLOR_PAIR(high > 0 ? TUI_COLOR_ERROR : TUI_COLOR_NORMAL));
    
    mvwprintw(win, *row, 45, "MED:");
    wattron(win, COLOR_PAIR(medium > 0 ? TUI_COLOR_WARNING : TUI_COLOR_NORMAL));
    wprintw(win, "%d", medium);
    wattroff(win, COLOR_PAIR(medium > 0 ? TUI_COLOR_WARNING : TUI_COLOR_NORMAL));
    
    mvwprintw(win, *row, 54, "LOW:");
    wattron(win, COLOR_PAIR(low > 0 ? TUI_COLOR_STATUS : TUI_COLOR_NORMAL));
    wprintw(win, "%d", low);
    wattroff(win, COLOR_PAIR(low > 0 ? TUI_COLOR_STATUS : TUI_COLOR_NORMAL));
    
    (*row) += 2;
}

static void draw_alarm_list(WINDOW *win, int *row) {
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, *row, 2, "Active Alarms");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    (*row)++;
    
    mvwhline(win, *row, 2, ACS_HLINE, getmaxx(win) - 4);
    (*row)++;
    
    // Header
    wattron(win, A_BOLD);
    mvwprintw(win, *row, 4, "%-5s %-6s %-8s %-19s %-40s",
              "ID", "Sev", "State", "Time", "Message");
    wattroff(win, A_BOLD);
    (*row)++;
    
    if (g_page.alarm_count == 0) {
        (*row)++;
        wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
        mvwprintw(win, *row, 6, "No active alarms");
        wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
        return;
    }
    
    int visible = MIN(VISIBLE_ROWS, g_page.alarm_count - g_page.scroll_offset);
    
    for (int i = 0; i < visible; i++) {
        int idx = g_page.scroll_offset + i;
        alarm_display_t *a = &g_page.alarms[idx];
        
        if (idx == g_page.selected) {
            wattron(win, A_REVERSE);
        }
        
        int color = severity_to_color(a->severity);
        
        // Time string
        char time_str[20];
        struct tm *tm = localtime(&a->raised_time);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
        
        mvwprintw(win, *row, 4, "%-5d ", a->id);
        
        wattron(win, COLOR_PAIR(color) | A_BOLD);
        wprintw(win, "%-6s", severity_to_str(a->severity));
        wattroff(win, COLOR_PAIR(color) | A_BOLD);
        
        wprintw(win, " ");
        
        int state_color = a->state == ALARM_STATE_ACTIVE ? TUI_COLOR_ERROR : TUI_COLOR_WARNING;
        wattron(win, COLOR_PAIR(state_color));
        wprintw(win, "%-8s", state_to_str(a->state));
        wattroff(win, COLOR_PAIR(state_color));
        
        wprintw(win, " %-19s ", time_str);
        
        // Truncate message if needed
        char msg_truncated[41];
        strncpy(msg_truncated, a->message, 40);
        msg_truncated[40] = '\0';
        wprintw(win, "%-40s", msg_truncated);
        
        if (idx == g_page.selected) {
            wattroff(win, A_REVERSE);
        }
        
        (*row)++;
    }
}

static void draw_help(WINDOW *win) {
    int max_y = getmaxy(win);
    int row = max_y - 3;
    
    mvwhline(win, row++, 2, ACS_HLINE, getmaxx(win) - 4);
    wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwprintw(win, row, 2, "a:Ack  A:Ack All  Enter:Details  r:Refresh  Up/Down:Navigate");
    wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
}

static void show_alarm_details(void) {
    if (g_page.selected >= g_page.alarm_count) return;
    
    alarm_display_t *a = &g_page.alarms[g_page.selected];
    
    WINDOW *dialog = newwin(14, 70, 5, 5);
    box(dialog, 0, 0);
    
    wattron(dialog, A_BOLD);
    mvwprintw(dialog, 0, 2, " Alarm Details ");
    wattroff(dialog, A_BOLD);
    
    int row = 2;
    mvwprintw(dialog, row++, 2, "Alarm ID:    %d", a->id);
    mvwprintw(dialog, row++, 2, "Rule ID:     %d", a->rule_id);
    mvwprintw(dialog, row++, 2, "Module ID:   %d", a->module_id);
    
    mvwprintw(dialog, row, 2, "Severity:    ");
    wattron(dialog, COLOR_PAIR(severity_to_color(a->severity)) | A_BOLD);
    wprintw(dialog, "%s", alarm_severity_to_string(a->severity));
    wattroff(dialog, COLOR_PAIR(severity_to_color(a->severity)) | A_BOLD);
    row++;
    
    mvwprintw(dialog, row++, 2, "State:       %s", alarm_state_to_string(a->state));
    mvwprintw(dialog, row++, 2, "Value:       %.4f", a->trigger_value);
    
    char time_str[32];
    struct tm *tm = localtime(&a->raised_time);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
    mvwprintw(dialog, row++, 2, "Raised:      %s", time_str);
    
    if (a->acknowledged_by[0]) {
        mvwprintw(dialog, row++, 2, "Ack By:      %s", a->acknowledged_by);
    }
    
    row++;
    mvwprintw(dialog, row++, 2, "Message: %s", a->message);
    
    mvwprintw(dialog, 12, 2, "Press any key to close");
    
    wrefresh(dialog);
    wgetch(dialog);
    delwin(dialog);
}

static void acknowledge_selected(void) {
    if (g_page.selected >= g_page.alarm_count) return;
    
    alarm_display_t *a = &g_page.alarms[g_page.selected];
    
    if (a->state == ALARM_STATE_ACTIVE) {
        if (alarm_manager_acknowledge(a->id, "operator") == RESULT_OK) {
            tui_set_status("Acknowledged alarm %d", a->id);
            load_active_alarms();
        } else {
            tui_set_status("Failed to acknowledge alarm");
        }
    }
}

static void acknowledge_all(void) {
    if (alarm_manager_acknowledge_all("operator") == RESULT_OK) {
        tui_set_status("Acknowledged all alarms");
        load_active_alarms();
    } else {
        tui_set_status("Failed to acknowledge alarms");
    }
}

void page_alarms_init(WINDOW *win) {
    g_page.win = win;
    g_page.selected = 0;
    g_page.scroll_offset = 0;
    g_page.view_mode = 0;
    load_active_alarms();
}

void page_alarms_draw(WINDOW *win) {
    int row = 2;
    
    draw_alarm_summary(win, &row);
    draw_alarm_list(win, &row);
    draw_help(win);
}

void page_alarms_input(WINDOW *win, int ch) {
    UNUSED(win);
    
    switch (ch) {
        case KEY_UP:
            if (g_page.selected > 0) {
                g_page.selected--;
                if (g_page.selected < g_page.scroll_offset) {
                    g_page.scroll_offset = g_page.selected;
                }
            }
            break;
            
        case KEY_DOWN:
            if (g_page.selected < g_page.alarm_count - 1) {
                g_page.selected++;
                if (g_page.selected >= g_page.scroll_offset + VISIBLE_ROWS) {
                    g_page.scroll_offset = g_page.selected - VISIBLE_ROWS + 1;
                }
            }
            break;
            
        case KEY_PPAGE:
            g_page.selected = MAX(0, g_page.selected - VISIBLE_ROWS);
            g_page.scroll_offset = MAX(0, g_page.scroll_offset - VISIBLE_ROWS);
            break;
            
        case KEY_NPAGE:
            g_page.selected = MIN(g_page.alarm_count - 1, g_page.selected + VISIBLE_ROWS);
            g_page.scroll_offset = MIN(MAX(0, g_page.alarm_count - VISIBLE_ROWS),
                                       g_page.scroll_offset + VISIBLE_ROWS);
            break;
            
        case '\n':
        case KEY_ENTER:
            if (g_page.alarm_count > 0) {
                show_alarm_details();
            }
            break;
            
        case 'a':
            acknowledge_selected();
            break;
            
        case 'A':
            acknowledge_all();
            break;
            
        case 'r':
        case 'R':
            load_active_alarms();
            tui_set_status("Refreshed %d alarms", g_page.alarm_count);
            break;
    }
}

void page_alarms_cleanup(void) {
    g_page.win = NULL;
}
