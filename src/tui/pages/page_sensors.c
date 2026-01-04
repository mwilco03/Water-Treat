/**
 * @file page_sensors.c
 * @brief Sensor management page
 */

#include "page_sensors.h"
#include "../tui_common.h"
#include "../dialogs/dialog_sensor.h"
#include "../dialogs/dialog_io_wizard.h"
#include "db/database.h"
#include "db/db_modules.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>

#define MAX_SENSORS 64
#define VISIBLE_ROWS 15

typedef struct {
    int id;
    int slot;
    char name[64];
    char type[32];
    char status[16];
    float value;
    char unit[16];
} sensor_item_t;

static struct {
    WINDOW *win;
    sensor_item_t sensors[MAX_SENSORS];
    tui_list_state_t list;    /* Reusable list widget for navigation */
    bool show_dialog;
    int dialog_type;  // 0=view, 1=add, 2=edit, 3=delete
} g_page = {0};

static void load_sensors(void) {
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

    for (int i = 0; i < count && i < MAX_SENSORS; i++) {
        sensor_item_t *s = &g_page.sensors[sensor_count];
        s->id = modules[i].module.id;
        s->slot = modules[i].module.slot;
        SAFE_STRNCPY(s->name, modules[i].module.name, sizeof(s->name));
        SAFE_STRNCPY(s->type, modules[i].module.module_type, sizeof(s->type));
        /* Value and status come from JOIN - no additional query needed */
        s->value = modules[i].value;
        SAFE_STRNCPY(s->status, modules[i].sensor_status, sizeof(s->status));

        sensor_count++;
    }

    free(modules);
    tui_list_set_count(&g_page.list, sensor_count);
}

static void draw_sensor_list(WINDOW *win) {
    int row = 3;
    int max_y, max_x;
    getmaxyx(win, max_y, max_x);
    UNUSED(max_y);

    // Header
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, row++, 2, "%-4s %-20s %-12s %-10s %-12s %-8s",
              "Slot", "Name", "Type", "Value", "Status", "");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));

    mvwhline(win, row++, 2, ACS_HLINE, max_x - 4);

    if (g_page.list.item_count == 0) {
        wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
        mvwprintw(win, row + 2, 4, "No sensors configured. Press 'a' to add a sensor.");
        wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
        return;
    }

    // Sensor list - use list widget helper
    int visible = tui_list_visible_count(&g_page.list);

    for (int i = 0; i < visible; i++) {
        int idx = g_page.list.scroll_offset + i;
        sensor_item_t *s = &g_page.sensors[idx];

        if (idx == g_page.list.selected) {
            wattron(win, A_REVERSE);
        }

        /* Use centralized status color function */
        int color = tui_status_color(s->status);

        mvwprintw(win, row, 2, "%-4d %-20s %-12s ", s->slot, s->name, s->type);

        // Value
        wattron(win, COLOR_PAIR(color));
        wprintw(win, "%-10.2f ", s->value);
        wattroff(win, COLOR_PAIR(color));

        // Status
        wattron(win, COLOR_PAIR(color));
        wprintw(win, "%-12s", s->status);
        wattroff(win, COLOR_PAIR(color));

        if (idx == g_page.list.selected) {
            wattroff(win, A_REVERSE);
        }

        row++;
    }

    // Scroll indicator - use list widget helper
    if (g_page.list.item_count > VISIBLE_ROWS) {
        mvwprintw(win, 3, max_x - 8, "[%3d%%]", tui_list_scroll_percent(&g_page.list));
    }
}

static void draw_help(WINDOW *win) {
    int max_y = getmaxy(win);
    int row = max_y - 4;
    
    wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwhline(win, row++, 2, ACS_HLINE, getmaxx(win) - 4);
    mvwprintw(win, row++, 2, "a:Add  e:Edit  d:Delete  Enter:View  r:Refresh  Arrows:Navigate");
    wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
}

static void show_view_dialog(void) {
    database_t *db = tui_get_database();
    if (!db || g_page.list.selected >= g_page.list.item_count) return;

    sensor_item_t *s = &g_page.sensors[g_page.list.selected];

    WINDOW *dialog = newwin(20, 60, 4, 10);
    box(dialog, 0, 0);

    wattron(dialog, A_BOLD);
    mvwprintw(dialog, 0, 20, " Sensor Details ");
    wattroff(dialog, A_BOLD);

    int row = 2;
    mvwprintw(dialog, row++, 2, "Slot:     %d", s->slot);
    mvwprintw(dialog, row++, 2, "Name:     %s", s->name);
    mvwprintw(dialog, row++, 2, "Type:     %s", s->type);
    mvwprintw(dialog, row++, 2, "Value:    %.4f", s->value);
    mvwprintw(dialog, row++, 2, "Status:   %s", s->status);

    row++;

    /* Get sensor-specific details */
    db_physical_sensor_t phys;
    if (db_physical_sensor_get(db, s->id, &phys) == RESULT_OK) {
        mvwprintw(dialog, row++, 2, "Interface: %s", phys.interface);
        mvwprintw(dialog, row++, 2, "Address:   %s", phys.address);
        mvwprintw(dialog, row++, 2, "Bus:       %d", phys.bus);
        mvwprintw(dialog, row++, 2, "Channel:   %d", phys.channel);
        mvwprintw(dialog, row++, 2, "Poll Rate: %d ms", phys.poll_rate_ms);
    }

    db_adc_sensor_t adc;
    if (db_adc_sensor_get(db, s->id, &adc) == RESULT_OK) {
        mvwprintw(dialog, row++, 2, "ADC Type:  %s", adc.adc_type);
        mvwprintw(dialog, row++, 2, "Channel:   %d", adc.channel);
        mvwprintw(dialog, row++, 2, "Gain:      %d", adc.gain);
        mvwprintw(dialog, row++, 2, "Range:     %.2f - %.2f %s", adc.eng_min, adc.eng_max, adc.unit);
    }

    row++;
    wattron(dialog, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwprintw(dialog, row, 2, "Press any key to close");
    wattroff(dialog, COLOR_PAIR(TUI_COLOR_NORMAL));

    wrefresh(dialog);
    wgetch(dialog);
    delwin(dialog);
}

static void handle_add_sensor(void) {
    /*
     * Use the new progressive disclosure I/O wizard.
     *
     * Design Philosophy Applied:
     * - Dynamic Discovery: Wizard scans I2C/1-Wire before asking questions
     * - Reasonable Assumptions: System infers technical details from user choices
     * - Graceful Degradation: Conflicts shown, not blocked
     * - Single Source of Truth: User points at device, system derives config
     * - Informational Output: Shows what was discovered
     *
     * The old dialog_sensor_add() is still available for power users
     * who need to configure advanced settings.
     */
    io_wizard_result_t result;
    if (dialog_io_wizard_add_sensor(&result)) {
        /* Sensor was created - reload and notify */
        load_sensors();
        tui_notify_sensor_changed(-1);  /* -1 = all sensors changed */
        tui_set_status("Added sensor '%s' at slot %d", result.name, result.assigned_slot);

        /* Select the newly added sensor */
        for (int i = 0; i < g_page.list.item_count; i++) {
            if (g_page.sensors[i].id == result.created_id) {
                g_page.list.selected = i;
                break;
            }
        }
    }
}

static void handle_edit_sensor(void) {
    if (g_page.list.selected >= g_page.list.item_count) return;

    sensor_item_t *s = &g_page.sensors[g_page.list.selected];
    int slot = s->slot;

    if (dialog_sensor_edit(s->id)) {
        /* Sensor was updated - reload and notify */
        load_sensors();
        tui_notify_sensor_changed(slot);
        tui_set_status("Updated sensor: %s", s->name);
    }
}

static void handle_delete_sensor(void) {
    if (g_page.list.selected >= g_page.list.item_count) return;

    sensor_item_t *s = &g_page.sensors[g_page.list.selected];
    int slot = s->slot;
    char name[64];
    SAFE_STRNCPY(name, s->name, sizeof(name));

    if (dialog_sensor_delete(s->id)) {
        /* Sensor was deleted - reload and notify */
        load_sensors();
        tui_notify_sensor_changed(slot);
        tui_set_status("Deleted sensor: %s", name);
        /* tui_list_set_count() in load_sensors() already adjusts selection */
    }
}

void page_sensors_init(WINDOW *win) {
    g_page.win = win;
    tui_list_init(&g_page.list, VISIBLE_ROWS);
    load_sensors();
}

void page_sensors_draw(WINDOW *win) {
    draw_sensor_list(win);
    draw_help(win);
}

void page_sensors_input(WINDOW *win, int ch) {
    UNUSED(win);

    /* Let list widget handle navigation keys */
    if (tui_list_input(&g_page.list, ch)) {
        return;
    }

    /* Handle page-specific keys */
    switch (ch) {
        case '\n':
        case KEY_ENTER:
            if (g_page.list.item_count > 0) {
                show_view_dialog();
            }
            break;

        case 'a':
        case 'A':
            handle_add_sensor();
            break;

        case 'e':
        case 'E':
            if (g_page.list.item_count > 0) {
                handle_edit_sensor();
            }
            break;

        case 'd':
        case 'D':
            if (g_page.list.item_count > 0) {
                handle_delete_sensor();
            }
            break;

        case 'r':
        case 'R':
            load_sensors();
            tui_set_status("Refreshed %d sensors", g_page.list.item_count);
            break;
    }
}

void page_sensors_cleanup(void) {
    g_page.win = NULL;
}
