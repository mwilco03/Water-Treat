/**
 * @file page_actuators.c
 * @brief Actuator management and manual control page
 *
 * Provides TUI interface for:
 * - Viewing actuator status
 * - Manual ON/OFF control
 * - PWM duty cycle adjustment
 * - Adding/editing/deleting actuators
 * - Emergency stop
 */

#include "page_actuators.h"
#include "../tui_common.h"
#include "tui/dialogs/dialog_actuator.h"
#include "db/database.h"
#include "db/db_actuators.h"
#include "actuators/actuator_manager.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>

#define MAX_ACTUATORS 32
#define VISIBLE_ROWS 12

/* External actuator manager from main.c */
extern actuator_manager_t g_actuator_mgr;

typedef struct {
    int id;
    int slot;
    char name[64];
    char type[32];
    int gpio_pin;
    bool state;           /* Current ON/OFF state */
    uint8_t pwm_duty;     /* PWM duty cycle 0-100 */
    bool manual_mode;     /* In manual control mode */
    bool fault;           /* Fault detected */
    char status[16];
} actuator_item_t;

static struct {
    WINDOW *win;
    actuator_item_t actuators[MAX_ACTUATORS];
    int actuator_count;
    int selected;
    int scroll_offset;
} g_page = {0};

static void load_actuators(void) {
    g_page.actuator_count = 0;

    database_t *db = tui_get_database();
    if (!db) return;

    db_actuator_t *actuators = NULL;
    int count = 0;

    if (db_actuator_list(db, &actuators, &count) != RESULT_OK || !actuators) {
        return;
    }

    for (int i = 0; i < count && i < MAX_ACTUATORS; i++) {
        actuator_item_t *a = &g_page.actuators[g_page.actuator_count];
        a->id = actuators[i].id;
        a->slot = actuators[i].slot;
        SAFE_STRNCPY(a->name, actuators[i].name, sizeof(a->name));
        a->gpio_pin = actuators[i].gpio_pin;

        /* Map type */
        switch (actuators[i].type) {
            case ACTUATOR_TYPE_PUMP:
                SAFE_STRNCPY(a->type, "Pump", sizeof(a->type));
                break;
            case ACTUATOR_TYPE_VALVE:
                SAFE_STRNCPY(a->type, "Valve", sizeof(a->type));
                break;
            case ACTUATOR_TYPE_RELAY:
                SAFE_STRNCPY(a->type, "Relay", sizeof(a->type));
                break;
            default:
                SAFE_STRNCPY(a->type, "Unknown", sizeof(a->type));
        }

        /* Get current state from actuator manager */
        actuator_state_t state;
        uint8_t pwm_duty = 0;
        if (actuator_manager_get_state(&g_actuator_mgr, a->slot, &state, &pwm_duty) == RESULT_OK) {
            a->state = (state == ACTUATOR_STATE_ON);
            a->fault = (state == ACTUATOR_STATE_FAULT);
            a->manual_mode = false;  /* TODO: track manual mode */
            a->pwm_duty = pwm_duty;
        } else {
            a->state = false;
            a->fault = false;
            a->manual_mode = false;
            a->pwm_duty = 0;
        }

        /* Set status string */
        if (a->fault) {
            SAFE_STRNCPY(a->status, "FAULT", sizeof(a->status));
        } else if (a->state) {
            SAFE_STRNCPY(a->status, "ON", sizeof(a->status));
        } else {
            SAFE_STRNCPY(a->status, "OFF", sizeof(a->status));
        }

        g_page.actuator_count++;
    }

    free(actuators);
}

static void draw_actuator_list(WINDOW *win) {
    int row = 3;
    int max_y, max_x;
    getmaxyx(win, max_y, max_x);
    UNUSED(max_y);

    /* Header */
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, row++, 2, "%-4s %-20s %-10s %-6s %-8s %-6s %-10s",
              "Slot", "Name", "Type", "GPIO", "State", "PWM", "Mode");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));

    mvwhline(win, row++, 2, ACS_HLINE, max_x - 4);

    if (g_page.actuator_count == 0) {
        wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
        mvwprintw(win, row + 2, 4, "No actuators configured. Press 'a' to add an actuator.");
        wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
        return;
    }

    int visible = MIN(VISIBLE_ROWS, g_page.actuator_count - g_page.scroll_offset);

    for (int i = 0; i < visible; i++) {
        int idx = g_page.scroll_offset + i;
        actuator_item_t *a = &g_page.actuators[idx];

        if (idx == g_page.selected) {
            wattron(win, A_REVERSE);
        }

        /* Status color */
        int color = TUI_COLOR_NORMAL;
        if (a->fault) {
            color = TUI_COLOR_ERROR;
        } else if (a->state) {
            color = TUI_COLOR_STATUS;
        }

        mvwprintw(win, row, 2, "%-4d %-20s %-10s %-6d ",
                  a->slot, a->name, a->type, a->gpio_pin);

        /* State with color */
        wattron(win, COLOR_PAIR(color) | A_BOLD);
        wprintw(win, "%-8s", a->status);
        wattroff(win, COLOR_PAIR(color) | A_BOLD);

        /* PWM */
        wprintw(win, "%-6d ", a->pwm_duty);

        /* Mode */
        if (a->manual_mode) {
            wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
            wprintw(win, "MANUAL");
            wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
        } else {
            wprintw(win, "AUTO");
        }

        if (idx == g_page.selected) {
            wattroff(win, A_REVERSE);
        }

        row++;
    }

    /* Degraded mode warning */
    if (g_actuator_mgr.degraded_mode) {
        row = max_y - 7;
        wattron(win, COLOR_PAIR(TUI_COLOR_WARNING) | A_BOLD);
        mvwprintw(win, row, 2, "⚠ DEGRADED MODE - Controller disconnected, holding last state");
        wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING) | A_BOLD);
    }
}

static void draw_help(WINDOW *win) {
    int max_y = getmaxy(win);
    int row = max_y - 4;

    wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwhline(win, row++, 2, ACS_HLINE, getmaxx(win) - 4);
    mvwprintw(win, row++, 2,
              "SPACE:Toggle  +/-:PWM  a:Add  e:Edit  d:Delete  E:Emergency Stop  r:Refresh");
    wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
}

static void toggle_actuator(void) {
    if (g_page.selected >= g_page.actuator_count) return;

    actuator_item_t *a = &g_page.actuators[g_page.selected];

    /* Toggle state using manual control */
    actuator_state_t new_state = a->state ? ACTUATOR_STATE_OFF : ACTUATOR_STATE_ON;
    result_t r = actuator_manager_manual_set(&g_actuator_mgr, a->slot, new_state, a->pwm_duty);

    if (r == RESULT_OK) {
        a->state = (new_state == ACTUATOR_STATE_ON);
        a->manual_mode = true;
        SAFE_STRNCPY(a->status, a->state ? "ON" : "OFF", sizeof(a->status));
        tui_set_status("%s %s: %s", a->type, a->name, a->state ? "ON" : "OFF");
        LOG_INFO("Manual actuator control: slot=%d %s", a->slot, a->state ? "ON" : "OFF");
    } else {
        tui_set_status("Failed to control %s", a->name);
    }
}

static void adjust_pwm(int delta) {
    if (g_page.selected >= g_page.actuator_count) return;

    actuator_item_t *a = &g_page.actuators[g_page.selected];

    int new_duty = a->pwm_duty + delta;
    if (new_duty < 0) new_duty = 0;
    if (new_duty > 100) new_duty = 100;

    actuator_state_t state = a->state ? ACTUATOR_STATE_ON : ACTUATOR_STATE_OFF;
    result_t r = actuator_manager_manual_set(&g_actuator_mgr, a->slot, state, (uint8_t)new_duty);

    if (r == RESULT_OK) {
        a->pwm_duty = new_duty;
        a->manual_mode = true;
        tui_set_status("%s PWM: %d%%", a->name, new_duty);
    }
}

static void emergency_stop(void) {
    /* Confirm emergency stop */
    WINDOW *dialog = newwin(7, 50, 10, 15);
    box(dialog, 0, 0);

    wattron(dialog, A_BOLD | COLOR_PAIR(TUI_COLOR_ERROR));
    mvwprintw(dialog, 0, 12, " EMERGENCY STOP ");
    wattroff(dialog, A_BOLD | COLOR_PAIR(TUI_COLOR_ERROR));

    mvwprintw(dialog, 2, 2, "This will turn OFF all actuators!");
    mvwprintw(dialog, 4, 2, "Press 'Y' to confirm, any other key to cancel");
    wrefresh(dialog);

    int ch = wgetch(dialog);
    delwin(dialog);

    if (ch == 'Y' || ch == 'y') {
        result_t r = actuator_manager_emergency_stop(&g_actuator_mgr);
        if (r == RESULT_OK) {
            tui_set_status("EMERGENCY STOP - All actuators OFF");
            LOG_WARNING("Emergency stop activated via TUI");
            load_actuators();  /* Refresh state */
        } else {
            tui_set_status("Emergency stop failed!");
        }
    }
}

static void show_add_dialog(void) {
    WINDOW *dialog = newwin(16, 55, 4, 12);
    box(dialog, 0, 0);

    wattron(dialog, A_BOLD);
    mvwprintw(dialog, 0, 18, " Add Actuator ");
    wattroff(dialog, A_BOLD);

    /* Form fields */
    char name[64] = "New Actuator";
    int slot = 10;
    int gpio_pin = 17;
    int type_sel = 0;
    const char *types[] = {"Pump", "Valve", "Relay"};

    int field = 0;
    bool done = false;

    while (!done) {
        /* Draw form */
        mvwprintw(dialog, 2, 2, "Name:      ");
        wattron(dialog, field == 0 ? A_REVERSE : 0);
        mvwprintw(dialog, 2, 13, "%-30s", name);
        wattroff(dialog, A_REVERSE);

        mvwprintw(dialog, 4, 2, "Slot:      ");
        wattron(dialog, field == 1 ? A_REVERSE : 0);
        mvwprintw(dialog, 4, 13, "%-10d", slot);
        wattroff(dialog, A_REVERSE);

        mvwprintw(dialog, 6, 2, "GPIO Pin:  ");
        wattron(dialog, field == 2 ? A_REVERSE : 0);
        mvwprintw(dialog, 6, 13, "%-10d", gpio_pin);
        wattroff(dialog, A_REVERSE);

        mvwprintw(dialog, 8, 2, "Type:      ");
        wattron(dialog, field == 3 ? A_REVERSE : 0);
        mvwprintw(dialog, 8, 13, "%-10s", types[type_sel]);
        wattroff(dialog, A_REVERSE);

        mvwprintw(dialog, 12, 2, "Enter: Edit field  Tab: Next  Esc: Cancel");
        mvwprintw(dialog, 13, 2, "Ctrl+S: Save");

        wrefresh(dialog);

        int ch = wgetch(dialog);
        switch (ch) {
            case KEY_UP:
                if (field > 0) field--;
                break;
            case KEY_DOWN:
            case '\t':
                if (field < 3) field++;
                else field = 0;
                break;
            case '\n':
            case KEY_ENTER:
                if (field == 0) {
                    tui_get_string(dialog, 2, 13, name, 30, name);
                } else if (field == 1) {
                    tui_get_int(dialog, 4, 13, &slot, 1, 63);
                } else if (field == 2) {
                    tui_get_int(dialog, 6, 13, &gpio_pin, 0, 27);
                } else if (field == 3) {
                    type_sel = (type_sel + 1) % 3;
                }
                break;
            case 19:  /* Ctrl+S */
                {
                    database_t *db = tui_get_database();
                    if (db) {
                        db_actuator_t act = {0};
                        SAFE_STRNCPY(act.name, name, sizeof(act.name));
                        act.slot = slot;
                        act.gpio_pin = gpio_pin;
                        act.type = type_sel;
                        act.active_low = false;
                        act.pwm_frequency_hz = (type_sel == ACTUATOR_TYPE_PWM) ? 1000 : 0;

                        int new_id;
                        if (db_actuator_create(db, &act, &new_id) == RESULT_OK) {
                            tui_set_status("Added actuator: %s", name);
                            load_actuators();
                            done = true;
                        } else {
                            tui_set_status("Failed to add actuator");
                        }
                    }
                }
                break;
            case 27:  /* Escape */
                done = true;
                break;
        }
    }

    delwin(dialog);
}

void page_actuators_init(WINDOW *win) {
    g_page.win = win;
    g_page.selected = 0;
    g_page.scroll_offset = 0;
    load_actuators();
}

void page_actuators_draw(WINDOW *win) {
    draw_actuator_list(win);
    draw_help(win);
}

void page_actuators_input(WINDOW *win, int ch) {
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
            if (g_page.selected < g_page.actuator_count - 1) {
                g_page.selected++;
                if (g_page.selected >= g_page.scroll_offset + VISIBLE_ROWS) {
                    g_page.scroll_offset = g_page.selected - VISIBLE_ROWS + 1;
                }
            }
            break;

        case ' ':  /* Space - toggle ON/OFF */
            toggle_actuator();
            break;

        case '+':
        case '=':
            adjust_pwm(10);
            break;

        case '-':
        case '_':
            adjust_pwm(-10);
            break;

        case 'a':
        case 'n':
        case 'N':
            /* Add new actuator */
            {
                database_t *db = tui_get_database();
                if (db) {
                    actuator_form_t form;
                    dialog_actuator_init_form(&form);
                    if (dialog_actuator_show(ACTUATOR_DIALOG_ADD, &form)) {
                        db_actuator_t db_act = {0};
                        dialog_actuator_save(&form, &db_act);
                        int new_id = 0;
                        if (db_actuator_create(db, &db_act, &new_id) == RESULT_OK) {
                            tui_set_status("Actuator '%s' created (ID: %d)", form.name, new_id);
                            load_actuators();
                        } else {
                            tui_set_status("Failed to create actuator");
                        }
                    }
                }
            }
            break;

        case 'e':
        case '\n':
        case KEY_ENTER:
            /* Edit selected actuator */
            if (g_page.actuator_count > 0) {
                database_t *db = tui_get_database();
                if (db) {
                    actuator_item_t *item = &g_page.actuators[g_page.selected];
                    db_actuator_t db_act = {0};
                    if (db_actuator_get(db, item->id, &db_act) == RESULT_OK) {
                        actuator_form_t form;
                        dialog_actuator_load(&form, &db_act);
                        if (dialog_actuator_show(ACTUATOR_DIALOG_EDIT, &form)) {
                            dialog_actuator_save(&form, &db_act);
                            if (db_actuator_update(db, &db_act) == RESULT_OK) {
                                tui_set_status("Actuator '%s' updated", form.name);
                                load_actuators();
                            } else {
                                tui_set_status("Failed to update actuator");
                            }
                        }
                    }
                }
            }
            break;

        case 'd':
        case KEY_DC:
            /* Delete selected actuator */
            if (g_page.actuator_count > 0) {
                database_t *db = tui_get_database();
                if (db) {
                    actuator_item_t *item = &g_page.actuators[g_page.selected];
                    if (dialog_actuator_confirm_delete(item->name)) {
                        if (db_actuator_delete(db, item->id) == RESULT_OK) {
                            tui_set_status("Actuator '%s' deleted", item->name);
                            load_actuators();
                            if (g_page.selected >= g_page.actuator_count && g_page.actuator_count > 0) {
                                g_page.selected = g_page.actuator_count - 1;
                            }
                        } else {
                            tui_set_status("Failed to delete actuator");
                        }
                    }
                }
            }
            break;

        case 'E':  /* Capital E for emergency stop */
            emergency_stop();
            break;

        case 'r':
        case 'R':
            load_actuators();
            tui_set_status("Refreshed %d actuators", g_page.actuator_count);
            break;
    }
}

void page_actuators_cleanup(void) {
    g_page.win = NULL;
}
