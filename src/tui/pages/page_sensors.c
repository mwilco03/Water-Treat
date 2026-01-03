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
    int sensor_count;
    int selected;
    int scroll_offset;
    bool show_dialog;
    int dialog_type;  // 0=view, 1=add, 2=edit, 3=delete
} g_page = {0};

static void load_sensors(void) {
    g_page.sensor_count = 0;
    
    database_t *db = tui_get_database();
    if (!db) return;
    
    db_module_t *modules = NULL;
    int count = 0;
    
    if (db_module_list(db, &modules, &count) != RESULT_OK || !modules) {
        return;
    }
    
    for (int i = 0; i < count && i < MAX_SENSORS; i++) {
        sensor_item_t *s = &g_page.sensors[g_page.sensor_count];
        s->id = modules[i].id;
        s->slot = modules[i].slot;
        SAFE_STRNCPY(s->name, modules[i].name, sizeof(s->name));
        SAFE_STRNCPY(s->type, modules[i].module_type, sizeof(s->type));
        SAFE_STRNCPY(s->status, modules[i].status, sizeof(s->status));
        
        // Get current value
        float value;
        char status[16];
        if (db_sensor_status_get(db, modules[i].id, &value, status, sizeof(status)) == RESULT_OK) {
            s->value = value;
            SAFE_STRNCPY(s->status, status, sizeof(s->status));
        }
        
        g_page.sensor_count++;
    }
    
    free(modules);
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
    
    if (g_page.sensor_count == 0) {
        wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
        mvwprintw(win, row + 2, 4, "No sensors configured. Press 'a' to add a sensor.");
        wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
        return;
    }
    
    // Sensor list
    int visible = MIN(VISIBLE_ROWS, g_page.sensor_count - g_page.scroll_offset);
    
    for (int i = 0; i < visible; i++) {
        int idx = g_page.scroll_offset + i;
        sensor_item_t *s = &g_page.sensors[idx];
        
        if (idx == g_page.selected) {
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
        
        if (idx == g_page.selected) {
            wattroff(win, A_REVERSE);
        }
        
        row++;
    }
    
    // Scroll indicator
    if (g_page.sensor_count > VISIBLE_ROWS) {
        int scroll_pct = (g_page.scroll_offset * 100) / (g_page.sensor_count - VISIBLE_ROWS);
        mvwprintw(win, 3, max_x - 8, "[%3d%%]", scroll_pct);
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
    if (!db || g_page.selected >= g_page.sensor_count) return;

    sensor_item_t *s = &g_page.sensors[g_page.selected];

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
        for (int i = 0; i < g_page.sensor_count; i++) {
            if (g_page.sensors[i].id == result.created_id) {
                g_page.selected = i;
                break;
            }
        }
    }
}

static void handle_edit_sensor(void) {
    if (g_page.selected >= g_page.sensor_count) return;

    sensor_item_t *s = &g_page.sensors[g_page.selected];
    int slot = s->slot;

    if (dialog_sensor_edit(s->id)) {
        /* Sensor was updated - reload and notify */
        load_sensors();
        tui_notify_sensor_changed(slot);
        tui_set_status("Updated sensor: %s", s->name);
    }
}

static void handle_delete_sensor(void) {
    if (g_page.selected >= g_page.sensor_count) return;

    sensor_item_t *s = &g_page.sensors[g_page.selected];
    int slot = s->slot;
    char name[64];
    SAFE_STRNCPY(name, s->name, sizeof(name));

    if (dialog_sensor_delete(s->id)) {
        /* Sensor was deleted - reload and notify */
        load_sensors();
        tui_notify_sensor_changed(slot);
        tui_set_status("Deleted sensor: %s", name);

        /* Adjust selection if needed */
        if (g_page.selected >= g_page.sensor_count && g_page.sensor_count > 0) {
            g_page.selected = g_page.sensor_count - 1;
        }
    }
}

void page_sensors_init(WINDOW *win) {
    g_page.win = win;
    g_page.selected = 0;
    g_page.scroll_offset = 0;
    load_sensors();
}

void page_sensors_draw(WINDOW *win) {
    draw_sensor_list(win);
    draw_help(win);
}

void page_sensors_input(WINDOW *win, int ch) {
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
            if (g_page.selected < g_page.sensor_count - 1) {
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
            g_page.selected = MIN(g_page.sensor_count - 1, g_page.selected + VISIBLE_ROWS);
            g_page.scroll_offset = MIN(MAX(0, g_page.sensor_count - VISIBLE_ROWS), 
                                       g_page.scroll_offset + VISIBLE_ROWS);
            break;
            
        case '\n':
        case KEY_ENTER:
            if (g_page.sensor_count > 0) {
                show_view_dialog();
            }
            break;

        case 'a':
        case 'A':
            handle_add_sensor();
            break;

        case 'e':
        case 'E':
            if (g_page.sensor_count > 0) {
                handle_edit_sensor();
            }
            break;

        case 'd':
        case 'D':
            if (g_page.sensor_count > 0) {
                handle_delete_sensor();
            }
            break;
            
        case 'r':
        case 'R':
            load_sensors();
            tui_set_status("Refreshed %d sensors", g_page.sensor_count);
            break;
    }
}

void page_sensors_cleanup(void) {
    g_page.win = NULL;
}
