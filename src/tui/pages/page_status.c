/**
 * @file page_status.c
 * @brief Real-time status overview page
 */

#include "page_status.h"
#include "../tui_common.h"
#include "db/database.h"
#include "db/db_modules.h"
#include "db/db_alarms.h"
#include "profinet/profinet_manager.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>
#include <sys/sysinfo.h>

#define MAX_DISPLAY_SENSORS 20
#define REFRESH_INTERVAL_MS 1000

typedef struct {
    int id;
    int slot;
    char name[32];
    float value;
    char unit[8];
    char status[16];
    time_t last_update;
} sensor_display_t;

/* Visible rows in sensor table (calculated from window size) */
#define STATUS_VISIBLE_ROWS 10

static struct {
    WINDOW *win;

    sensor_display_t sensors[MAX_DISPLAY_SENSORS];
    tui_list_state_t list;    /* Reusable list widget for scrolling */

    // System stats
    float cpu_temp;
    float memory_percent;
    long uptime_seconds;

    // PROFINET stats
    bool pn_connected;
    const char *pn_state;
    uint32_t pn_cycles;

    // Alarm stats
    int active_alarms;
    int critical_alarms;

    uint64_t last_refresh;
} g_page = {0};

static void refresh_sensor_data(void) {
    int sensor_count = 0;

    database_t *db = tui_get_database();
    if (!db) {
        tui_list_set_count(&g_page.list, 0);
        return;
    }

    /* Use optimized JOIN query - single query instead of N+1 */
    db_module_with_status_t *modules = NULL;
    int count = 0;

    if (db_module_list_with_status(db, &modules, &count) != RESULT_OK || !modules) {
        tui_list_set_count(&g_page.list, 0);
        return;
    }

    for (int i = 0; i < count && i < MAX_DISPLAY_SENSORS; i++) {
        sensor_display_t *s = &g_page.sensors[sensor_count];
        s->id = modules[i].module.id;
        s->slot = modules[i].module.slot;
        SAFE_STRNCPY(s->name, modules[i].module.name, sizeof(s->name));
        /* Value and status come from JOIN - no additional query needed */
        s->value = modules[i].value;
        SAFE_STRNCPY(s->status, modules[i].sensor_status, sizeof(s->status));

        sensor_count++;
    }

    free(modules);
    tui_list_set_count(&g_page.list, sensor_count);
}

static void refresh_system_stats(void) {
    // CPU Temperature
    FILE *fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (fp) {
        int temp;
        if (fscanf(fp, "%d", &temp) == 1) {
            g_page.cpu_temp = temp / 1000.0f;
        }
        fclose(fp);
    }
    
    // Memory
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        unsigned long total = si.totalram;
        unsigned long used = total - si.freeram;
        g_page.memory_percent = (float)used / total * 100.0f;
        g_page.uptime_seconds = si.uptime;
    }
}

static void refresh_profinet_stats(void) {
    g_page.pn_connected = profinet_manager_is_connected();
    g_page.pn_state = profinet_state_to_string(profinet_manager_get_state());
    
    profinet_stats_t stats;
    if (profinet_manager_get_stats(&stats) == RESULT_OK) {
        g_page.pn_cycles = stats.cycle_count;
    }
}

static void refresh_alarm_stats(void) {
    database_t *db = tui_get_database();
    if (!db) return;
    
    db_alarm_count_active(db, &g_page.active_alarms);
    db_alarm_count_by_severity(db, ALARM_SEVERITY_CRITICAL, &g_page.critical_alarms);
}

static void draw_header(WINDOW *win, int *row) {
    int max_x = getmaxx(win);
    
    // System status box
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, *row, 2, "System Status");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    (*row)++;
    
    mvwhline(win, *row, 2, ACS_HLINE, max_x - 4);
    (*row)++;
    
    // CPU Temp
    int color = g_page.cpu_temp < 60 ? TUI_COLOR_STATUS : 
                g_page.cpu_temp < 75 ? TUI_COLOR_WARNING : TUI_COLOR_ERROR;
    mvwprintw(win, *row, 4, "CPU: ");
    wattron(win, COLOR_PAIR(color));
    wprintw(win, "%.1f C", g_page.cpu_temp);
    wattroff(win, COLOR_PAIR(color));
    
    // Memory
    color = g_page.memory_percent < 70 ? TUI_COLOR_STATUS :
            g_page.memory_percent < 90 ? TUI_COLOR_WARNING : TUI_COLOR_ERROR;
    mvwprintw(win, *row, 20, "Memory: ");
    wattron(win, COLOR_PAIR(color));
    wprintw(win, "%.1f%%", g_page.memory_percent);
    wattroff(win, COLOR_PAIR(color));
    
    // Uptime
    int days = g_page.uptime_seconds / 86400;
    int hours = (g_page.uptime_seconds % 86400) / 3600;
    int mins = (g_page.uptime_seconds % 3600) / 60;
    mvwprintw(win, *row, 40, "Uptime: %dd %dh %dm", days, hours, mins);
    
    (*row) += 2;
}

static void draw_profinet_status(WINDOW *win, int *row) {
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, *row, 2, "PROFINET Status");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    (*row)++;
    
    mvwhline(win, *row, 2, ACS_HLINE, getmaxx(win) - 4);
    (*row)++;
    
    // Connection status
    int color = g_page.pn_connected ? TUI_COLOR_STATUS : TUI_COLOR_WARNING;
    mvwprintw(win, *row, 4, "State: ");
    wattron(win, COLOR_PAIR(color));
    wprintw(win, "%s", g_page.pn_state);
    wattroff(win, COLOR_PAIR(color));
    
    mvwprintw(win, *row, 30, "Cycles: %u", g_page.pn_cycles);
    
    // Alarms summary
    if (g_page.active_alarms > 0) {
        color = g_page.critical_alarms > 0 ? TUI_COLOR_ERROR : TUI_COLOR_WARNING;
        mvwprintw(win, *row, 50, "Alarms: ");
        wattron(win, COLOR_PAIR(color) | A_BOLD);
        wprintw(win, "%d", g_page.active_alarms);
        wattroff(win, COLOR_PAIR(color) | A_BOLD);
        if (g_page.critical_alarms > 0) {
            wattron(win, COLOR_PAIR(TUI_COLOR_ERROR));
            wprintw(win, " (%d CRIT)", g_page.critical_alarms);
            wattroff(win, COLOR_PAIR(TUI_COLOR_ERROR));
        }
    } else {
        mvwprintw(win, *row, 50, "Alarms: ");
        wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
        wprintw(win, "None");
        wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
    }
    
    (*row) += 2;
}

static void draw_sensor_table(WINDOW *win, int *row) {
    int max_x = getmaxx(win);
    int max_y = getmaxy(win);

    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, *row, 2, "Sensor Values (%d)", g_page.list.item_count);
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    (*row)++;

    mvwhline(win, *row, 2, ACS_HLINE, max_x - 4);
    (*row)++;

    // Header
    wattron(win, A_BOLD);
    mvwprintw(win, *row, 4, "%-4s %-24s %-12s %-10s", "Slot", "Name", "Value", "Status");
    wattroff(win, A_BOLD);
    (*row)++;

    if (g_page.list.item_count == 0) {
        wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
        mvwprintw(win, *row + 1, 6, "No sensors configured");
        wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
        return;
    }

    /* Use list widget for visible count */
    int visible = tui_list_visible_count(&g_page.list);
    visible = MIN(visible, max_y - *row - 4);

    for (int i = 0; i < visible; i++) {
        int idx = g_page.list.scroll_offset + i;
        sensor_display_t *s = &g_page.sensors[idx];

        int color = tui_status_color(s->status);

        mvwprintw(win, *row, 4, "%-4d %-24s ", s->slot, s->name);

        wattron(win, COLOR_PAIR(color));
        wprintw(win, "%-12.3f", s->value);
        wattroff(win, COLOR_PAIR(color));

        wattron(win, COLOR_PAIR(color));
        wprintw(win, "%-10s", s->status);
        wattroff(win, COLOR_PAIR(color));

        (*row)++;
    }
}

static void draw_help(WINDOW *win) {
    int max_y = getmaxy(win);
    int row = max_y - 2;
    
    wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwprintw(win, row, 2, "r:Refresh  Up/Down:Scroll  Space:Pause/Resume");
    wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
}

void page_status_init(WINDOW *win) {
    g_page.win = win;
    tui_list_init(&g_page.list, STATUS_VISIBLE_ROWS);
    g_page.last_refresh = 0;

    refresh_sensor_data();
    refresh_system_stats();
    refresh_profinet_stats();
    refresh_alarm_stats();
}

void page_status_draw(WINDOW *win) {
    uint64_t now = get_time_ms();
    if (now - g_page.last_refresh > REFRESH_INTERVAL_MS) {
        refresh_sensor_data();
        refresh_system_stats();
        refresh_profinet_stats();
        refresh_alarm_stats();
        g_page.last_refresh = now;
    }
    
    int row = 2;
    
    draw_header(win, &row);
    draw_profinet_status(win, &row);
    draw_sensor_table(win, &row);
    draw_help(win);
}

void page_status_input(WINDOW *win, int ch) {
    UNUSED(win);

    /* Let list widget handle navigation keys */
    if (tui_list_input(&g_page.list, ch)) {
        return;
    }

    /* Handle page-specific keys */
    switch (ch) {
        case 'r':
        case 'R':
            refresh_sensor_data();
            refresh_system_stats();
            refresh_profinet_stats();
            refresh_alarm_stats();
            g_page.last_refresh = get_time_ms();
            tui_set_status("Refreshed");
            break;
    }
}

void page_status_cleanup(void) {
    g_page.win = NULL;
}
