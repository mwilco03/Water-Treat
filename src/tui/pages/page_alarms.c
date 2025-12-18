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
#define MAX_RULES 64
#define VISIBLE_ROWS 10

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

typedef struct {
    int id;
    int module_id;
    char name[64];
    char sensor_name[64];
    alarm_condition_t condition;
    float threshold_high;
    float threshold_low;
    alarm_severity_t severity;
    bool enabled;
    bool interlock_enabled;
    int interlock_slot;
    interlock_action_t interlock_action;
    bool release_on_clear;
} rule_display_t;

static struct {
    WINDOW *win;

    alarm_display_t alarms[MAX_ALARMS];
    int alarm_count;

    rule_display_t rules[MAX_RULES];
    int rule_count;

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

static void load_alarm_rules(void) {
    g_page.rule_count = 0;

    database_t *db = tui_get_database();
    if (!db) return;

    db_alarm_rule_t *rules = NULL;
    int count = 0;

    if (db_alarm_rule_list(db, &rules, &count) != RESULT_OK || !rules) return;

    for (int i = 0; i < count && i < MAX_RULES; i++) {
        rule_display_t *r = &g_page.rules[g_page.rule_count];
        r->id = rules[i].id;
        r->module_id = rules[i].module_id;
        SAFE_STRNCPY(r->name, rules[i].name, sizeof(r->name));
        r->condition = rules[i].condition;
        r->threshold_high = rules[i].threshold_high;
        r->threshold_low = rules[i].threshold_low;
        r->severity = rules[i].severity;
        r->enabled = rules[i].enabled;
        r->interlock_enabled = rules[i].interlock_enabled;
        r->interlock_slot = rules[i].interlock_slot;
        r->interlock_action = rules[i].interlock_action;
        r->release_on_clear = rules[i].release_on_clear;

        /* Get sensor name from module */
        db_module_t mod;
        if (db_module_get(db, r->module_id, &mod) == RESULT_OK) {
            snprintf(r->sensor_name, sizeof(r->sensor_name), "%s (slot %d)", mod.name, mod.slot);
        } else {
            snprintf(r->sensor_name, sizeof(r->sensor_name), "Module %d", r->module_id);
        }

        g_page.rule_count++;
    }

    free(rules);
}

static const char* interlock_action_str(interlock_action_t action) {
    switch (action) {
        case INTERLOCK_ACTION_OFF: return "OFF";
        case INTERLOCK_ACTION_ON:  return "ON";
        case INTERLOCK_ACTION_PWM: return "PWM";
        default: return "-";
    }
}

static const char* condition_short_str(alarm_condition_t cond) {
    switch (cond) {
        case ALARM_CONDITION_ABOVE_THRESHOLD: return "Above";
        case ALARM_CONDITION_BELOW_THRESHOLD: return "Below";
        case ALARM_CONDITION_OUT_OF_RANGE:    return "Range";
        case ALARM_CONDITION_RATE_OF_CHANGE:  return "Rate";
        default: return "?";
    }
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

static void draw_view_tabs(WINDOW *win, int *row) {
    mvwprintw(win, *row, 2, "View: ");

    if (g_page.view_mode == 0) wattron(win, A_REVERSE);
    wprintw(win, " 1:Active ");
    if (g_page.view_mode == 0) wattroff(win, A_REVERSE);

    wprintw(win, " ");

    if (g_page.view_mode == 1) wattron(win, A_REVERSE);
    wprintw(win, " 2:History ");
    if (g_page.view_mode == 1) wattroff(win, A_REVERSE);

    wprintw(win, " ");

    if (g_page.view_mode == 2) wattron(win, A_REVERSE);
    wprintw(win, " 3:Rules ");
    if (g_page.view_mode == 2) wattroff(win, A_REVERSE);

    (*row) += 2;
}

static void draw_rules_list(WINDOW *win, int *row) {
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, *row, 2, "Alarm Rules Configuration");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    (*row)++;

    mvwhline(win, *row, 2, ACS_HLINE, getmaxx(win) - 4);
    (*row)++;

    /* Header */
    wattron(win, A_BOLD);
    mvwprintw(win, *row, 4, "%-3s %-20s %-6s %-8s %-6s %-12s %-4s",
              "ID", "Name", "Cond", "Thresh", "Sev", "Interlock", "On");
    wattroff(win, A_BOLD);
    (*row)++;

    if (g_page.rule_count == 0) {
        (*row)++;
        wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
        mvwprintw(win, *row, 6, "No alarm rules configured. Press 'n' to create one.");
        wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
        return;
    }

    int visible = MIN(VISIBLE_ROWS, g_page.rule_count - g_page.scroll_offset);

    for (int i = 0; i < visible; i++) {
        int idx = g_page.scroll_offset + i;
        rule_display_t *r = &g_page.rules[idx];

        if (idx == g_page.selected) {
            wattron(win, A_REVERSE);
        }

        int color = severity_to_color(r->severity);

        /* Format interlock info */
        char interlock_str[16];
        if (r->interlock_enabled) {
            snprintf(interlock_str, sizeof(interlock_str), "S%d:%s",
                     r->interlock_slot, interlock_action_str(r->interlock_action));
        } else {
            strcpy(interlock_str, "-");
        }

        /* Format threshold */
        char thresh_str[16];
        if (r->condition == ALARM_CONDITION_OUT_OF_RANGE) {
            snprintf(thresh_str, sizeof(thresh_str), "%.1f-%.1f", r->threshold_low, r->threshold_high);
        } else if (r->condition == ALARM_CONDITION_BELOW_THRESHOLD) {
            snprintf(thresh_str, sizeof(thresh_str), "%.1f", r->threshold_low);
        } else {
            snprintf(thresh_str, sizeof(thresh_str), "%.1f", r->threshold_high);
        }

        mvwprintw(win, *row, 4, "%-3d ", r->id);

        /* Name - truncate if needed */
        char name_trunc[21];
        strncpy(name_trunc, r->name[0] ? r->name : r->sensor_name, 20);
        name_trunc[20] = '\0';
        wprintw(win, "%-20s ", name_trunc);

        wprintw(win, "%-6s ", condition_short_str(r->condition));
        wprintw(win, "%-8s ", thresh_str);

        wattron(win, COLOR_PAIR(color) | A_BOLD);
        wprintw(win, "%-6s ", severity_to_str(r->severity));
        wattroff(win, COLOR_PAIR(color) | A_BOLD);

        if (r->interlock_enabled) {
            wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
        }
        wprintw(win, "%-12s ", interlock_str);
        if (r->interlock_enabled) {
            wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
        }

        wprintw(win, "%s", r->enabled ? "[X]" : "[ ]");

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

    if (g_page.view_mode == 2) {
        /* Rules view help */
        mvwprintw(win, row, 2, "n:New  Enter:Edit  d:Delete  e:Toggle  1/2/3:Views  r:Refresh");
    } else {
        /* Alarms view help */
        mvwprintw(win, row, 2, "a:Ack  A:Ack All  Enter:Details  1/2/3:Views  r:Refresh");
    }
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

    draw_view_tabs(win, &row);

    if (g_page.view_mode == 2) {
        /* Rules view */
        draw_rules_list(win, &row);
    } else {
        /* Active/History alarms view */
        draw_alarm_summary(win, &row);
        draw_alarm_list(win, &row);
    }

    draw_help(win);
}

static void toggle_rule_enabled(void) {
    if (g_page.view_mode != 2 || g_page.selected >= g_page.rule_count) return;

    rule_display_t *r = &g_page.rules[g_page.selected];
    database_t *db = tui_get_database();
    if (!db) return;

    bool new_state = !r->enabled;
    if (db_alarm_rule_set_enabled(db, r->id, new_state) == RESULT_OK) {
        r->enabled = new_state;
        tui_set_status("Rule %d %s", r->id, new_state ? "enabled" : "disabled");
    }
}

static void switch_view(int mode) {
    g_page.view_mode = mode;
    g_page.selected = 0;
    g_page.scroll_offset = 0;

    if (mode == 0 || mode == 1) {
        load_active_alarms();
    } else if (mode == 2) {
        load_alarm_rules();
    }
}

void page_alarms_input(WINDOW *win, int ch) {
    UNUSED(win);

    int item_count = (g_page.view_mode == 2) ? g_page.rule_count : g_page.alarm_count;

    switch (ch) {
        /* View switching */
        case '1':
            switch_view(0);
            tui_set_status("Active alarms view");
            break;

        case '2':
            switch_view(1);
            tui_set_status("Alarm history view");
            break;

        case '3':
            switch_view(2);
            tui_set_status("Alarm rules configuration (%d rules)", g_page.rule_count);
            break;

        /* Navigation */
        case KEY_UP:
            if (g_page.selected > 0) {
                g_page.selected--;
                if (g_page.selected < g_page.scroll_offset) {
                    g_page.scroll_offset = g_page.selected;
                }
            }
            break;

        case KEY_DOWN:
            if (g_page.selected < item_count - 1) {
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
            g_page.selected = MIN(item_count - 1, g_page.selected + VISIBLE_ROWS);
            g_page.scroll_offset = MIN(MAX(0, item_count - VISIBLE_ROWS),
                                       g_page.scroll_offset + VISIBLE_ROWS);
            break;

        /* Actions */
        case '\n':
        case KEY_ENTER:
            if (g_page.view_mode == 2) {
                /* TODO: Edit rule dialog */
                tui_set_status("Rule editing: Press 'e' to toggle, 'd' to delete");
            } else if (g_page.alarm_count > 0) {
                show_alarm_details();
            }
            break;

        case 'a':
            if (g_page.view_mode != 2) {
                acknowledge_selected();
            }
            break;

        case 'A':
            if (g_page.view_mode != 2) {
                acknowledge_all();
            }
            break;

        case 'e':
        case 'E':
            if (g_page.view_mode == 2) {
                toggle_rule_enabled();
            }
            break;

        case 'r':
        case 'R':
            if (g_page.view_mode == 2) {
                load_alarm_rules();
                tui_set_status("Refreshed %d rules", g_page.rule_count);
            } else {
                load_active_alarms();
                tui_set_status("Refreshed %d alarms", g_page.alarm_count);
            }
            break;
    }
}

void page_alarms_cleanup(void) {
    g_page.win = NULL;
}
