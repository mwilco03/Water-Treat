/**
 * @file page_profinet.c
 * @brief PROFINET I/O Device status and diagnostics page
 *
 * Displays real-time PROFINET connection status, I/O module mapping,
 * and cyclic data exchange statistics for this RTU.
 */

#include "page_profinet.h"
#include "../tui_common.h"
#include "profinet/profinet_manager.h"
#include "sensors/sensor_manager.h"
#include "actuators/actuator_manager.h"
#include "db/db_modules.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>

#define MAX_IO_SLOTS 16

typedef struct {
    int slot;
    int subslot;
    char name[32];
    char direction[8];  // "Input" or "Output"
    float value;
    bool valid;
} io_slot_info_t;

static struct {
    WINDOW *win;

    // PROFINET status
    profinet_state_t pn_state;
    bool pn_connected;
    uint32_t cycle_count;
    int plugged_modules;
    int total_modules;

    // I/O slot info
    io_slot_info_t slots[MAX_IO_SLOTS];
    int slot_count;
    int selected_slot;

    // Actuator status
    bool degraded_mode;
    int actuator_count;

    // Refresh timer
    uint64_t last_refresh;
} g_page = {0};

static void refresh_profinet_status(void) {
    profinet_stats_t stats;
    if (profinet_manager_get_stats(&stats) == RESULT_OK) {
        g_page.pn_state = stats.state;
        g_page.pn_connected = stats.connected;
        g_page.cycle_count = stats.cycle_count;
        g_page.plugged_modules = stats.plugged_modules;
        g_page.total_modules = stats.slot_count;
    }
}

static void refresh_io_slots(void) {
    database_t *db = tui_get_database();
    if (!db) return;

    g_page.slot_count = 0;

    // Get input modules (sensors)
    db_module_t *modules = NULL;
    int count = 0;

    if (db_module_list(db, &modules, &count) == RESULT_OK && modules) {
        for (int i = 0; i < count && g_page.slot_count < MAX_IO_SLOTS; i++) {
            io_slot_info_t *slot = &g_page.slots[g_page.slot_count];

            slot->slot = modules[i].slot;
            slot->subslot = modules[i].subslot;
            SAFE_STRNCPY(slot->name, modules[i].name, sizeof(slot->name));
            strcpy(slot->direction, "Input");

            // Get current value
            float value;
            char status[16];
            if (db_sensor_status_get(db, modules[i].id, &value, status, sizeof(status)) == RESULT_OK) {
                slot->value = value;
                slot->valid = (strcmp(status, "ok") == 0);
            } else {
                slot->value = 0.0f;
                slot->valid = false;
            }

            g_page.slot_count++;
        }
        free(modules);
    }

    // Add output modules (actuators)
    db_actuator_t *actuators = NULL;
    int act_count = 0;

    if (db_actuator_list(db, &actuators, &act_count) == RESULT_OK && actuators) {
        for (int i = 0; i < act_count && g_page.slot_count < MAX_IO_SLOTS; i++) {
            if (!actuators[i].enabled) continue;

            io_slot_info_t *slot = &g_page.slots[g_page.slot_count];

            slot->slot = actuators[i].slot;
            slot->subslot = actuators[i].subslot > 0 ? actuators[i].subslot : 1;
            SAFE_STRNCPY(slot->name, actuators[i].name, sizeof(slot->name));
            strcpy(slot->direction, "Output");

            // Get current state from actuator manager
            extern actuator_manager_t g_actuator_mgr;
            actuator_state_t state;
            uint8_t pwm_duty = 0;
            if (actuator_manager_get_state(&g_actuator_mgr, actuators[i].slot, &state, &pwm_duty) == RESULT_OK) {
                slot->value = (state == ACTUATOR_STATE_ON) ? 1.0f : 0.0f;
                slot->valid = (state != ACTUATOR_STATE_FAULT);
            } else {
                slot->value = 0.0f;
                slot->valid = false;
            }

            g_page.slot_count++;
        }
        free(actuators);
    }
}

static void draw_connection_status(WINDOW *win, int *row) {
    int r = *row;

    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, r++, 2, "PROFINET I/O Device Status");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    r++;

    // Connection state
    const char *state_str = profinet_state_to_string(g_page.pn_state);
    int state_color = TUI_COLOR_WARNING;
    if (g_page.pn_connected) {
        state_color = TUI_COLOR_STATUS;
    } else if (g_page.pn_state == PROFINET_STATE_ERROR) {
        state_color = TUI_COLOR_ERROR;
    }

    mvwprintw(win, r, 4, "Connection: ");
    wattron(win, COLOR_PAIR(state_color) | A_BOLD);
    wprintw(win, "%s", state_str);
    wattroff(win, COLOR_PAIR(state_color) | A_BOLD);
    r++;

    // Controller connection
    mvwprintw(win, r, 4, "Controller: ");
    if (g_page.pn_connected) {
        wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
        wprintw(win, "CONNECTED");
        wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
    } else {
        wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
        wprintw(win, "NOT CONNECTED");
        wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
    }
    r++;

    // Degraded mode warning
    if (g_page.degraded_mode) {
        wattron(win, COLOR_PAIR(TUI_COLOR_ERROR) | A_BLINK);
        mvwprintw(win, r, 4, "*** DEGRADED MODE - Maintaining last state ***");
        wattroff(win, COLOR_PAIR(TUI_COLOR_ERROR) | A_BLINK);
        r++;
    }
    r++;

    // Statistics
    wattron(win, A_UNDERLINE);
    mvwprintw(win, r++, 4, "Statistics");
    wattroff(win, A_UNDERLINE);

    mvwprintw(win, r++, 6, "Cycle Count:     %u", g_page.cycle_count);
    mvwprintw(win, r++, 6, "Plugged Modules: %d / %d",
              g_page.plugged_modules, g_page.total_modules);
    mvwprintw(win, r++, 6, "Actuators:       %d", g_page.actuator_count);

    *row = r + 1;
}

static void draw_io_slots(WINDOW *win, int *row) {
    int r = *row;
    int start_col = 2;

    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, r++, start_col, "I/O Module Map");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    r++;

    // Header
    wattron(win, A_BOLD);
    mvwprintw(win, r++, start_col, "%-6s %-20s %-8s %-12s %-6s",
              "Slot", "Name", "Dir", "Value", "Status");
    wattroff(win, A_BOLD);

    mvwhline(win, r++, start_col, ACS_HLINE, 60);

    if (g_page.slot_count == 0) {
        wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
        mvwprintw(win, r++, start_col + 2, "No I/O modules configured");
        wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
    }

    int max_visible = 10;
    for (int i = 0; i < MIN(max_visible, g_page.slot_count); i++) {
        io_slot_info_t *slot = &g_page.slots[i];

        if (i == g_page.selected_slot) {
            wattron(win, A_REVERSE);
        }

        mvwprintw(win, r, start_col, "%-6d %-20s %-8s ",
                  slot->slot, slot->name, slot->direction);

        // Value with color
        wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
        wprintw(win, "%-12.2f ", slot->value);
        wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));

        // Status indicator
        if (slot->valid) {
            wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
            wprintw(win, "OK");
            wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
        } else {
            wattron(win, COLOR_PAIR(TUI_COLOR_ERROR));
            wprintw(win, "BAD");
            wattroff(win, COLOR_PAIR(TUI_COLOR_ERROR));
        }

        if (i == g_page.selected_slot) {
            wattroff(win, A_REVERSE);
        }

        r++;
    }

    *row = r + 1;
}

static void draw_protocol_info(WINDOW *win, int start_row) {
    int r = start_row;
    int col = 50;

    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, r++, col, "Protocol Information");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    r++;

    mvwprintw(win, r++, col, "Protocol:   PROFINET I/O");
    mvwprintw(win, r++, col, "Role:       I/O Device (RTU)");
    mvwprintw(win, r++, col, "Vendor ID:  0x0493");
    mvwprintw(win, r++, col, "Device ID:  0x0001");
    r++;

    wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwprintw(win, r++, col, "Data flows to controller");
    mvwprintw(win, r++, col, "via clear-text PROFINET");
    mvwprintw(win, r++, col, "for network analysis.");
    wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
}

static void draw_help(WINDOW *win) {
    int max_y = getmaxy(win);
    int row = max_y - 3;

    mvwhline(win, row++, 2, ACS_HLINE, getmaxx(win) - 4);
    wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwprintw(win, row++, 2, "Up/Down:Select slot  r:Refresh  Space:Test selected");
    wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
}

void page_profinet_init(WINDOW *win) {
    g_page.win = win;
    g_page.selected_slot = 0;
    g_page.last_refresh = 0;

    refresh_profinet_status();
    refresh_io_slots();

    // Get actuator manager status
    extern actuator_manager_t g_actuator_mgr;
    g_page.degraded_mode = actuator_manager_is_degraded(&g_actuator_mgr);
    g_page.actuator_count = actuator_manager_get_count(&g_actuator_mgr);
}

void page_profinet_draw(WINDOW *win) {
    // Auto-refresh every second
    uint64_t now = get_time_ms();
    if (now - g_page.last_refresh > 1000) {
        refresh_profinet_status();
        refresh_io_slots();
        g_page.last_refresh = now;
    }

    int row = 3;
    draw_connection_status(win, &row);
    draw_io_slots(win, &row);
    draw_protocol_info(win, 3);
    draw_help(win);
}

void page_profinet_input(WINDOW *win, int ch) {
    UNUSED(win);

    switch (ch) {
        case KEY_UP:
            if (g_page.selected_slot > 0) {
                g_page.selected_slot--;
            }
            break;

        case KEY_DOWN:
            if (g_page.selected_slot < g_page.slot_count - 1) {
                g_page.selected_slot++;
            }
            break;

        case 'r':
        case 'R':
            refresh_profinet_status();
            refresh_io_slots();
            tui_set_status("Refreshed PROFINET status");
            break;

        case ' ':
            // Test selected sensor
            if (g_page.selected_slot < g_page.slot_count) {
                io_slot_info_t *slot = &g_page.slots[g_page.selected_slot];
                tui_set_status("Testing slot %d: %s", slot->slot, slot->name);
                // Could trigger sensor_manager_test_sensor here
            }
            break;
    }
}

void page_profinet_cleanup(void) {
    g_page.win = NULL;
}
