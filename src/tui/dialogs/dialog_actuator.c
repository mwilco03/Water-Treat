/**
 * @file dialog_actuator.c
 * @brief Actuator add/edit/delete dialog implementation
 */

#include "dialog_actuator.h"
#include "tui/tui_common.h"
#include "platform/board_detect.h"
#include "db/db_actuators.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Field indices */
enum {
    FIELD_NAME = 0,
    FIELD_SLOT,
    FIELD_SUBSLOT,
    FIELD_TYPE,
    FIELD_GPIO_PIN,
    FIELD_GPIO_CHIP,
    FIELD_ACTIVE_LOW,
    FIELD_PWM_FREQ,
    FIELD_SAFE_STATE,
    FIELD_MIN_ON_TIME,
    FIELD_MAX_ON_TIME,
    FIELD_ENABLED,
    FIELD_COUNT
};

/* Dialog state */
static struct {
    actuator_dialog_mode_t mode;
    actuator_form_t *form;
    int current_field;
    char edit_buffer[64];
    bool editing;
    bool confirmed;
} g_dlg = {0};

/* Type options - maps to actuator_type_t enum */
static const char *TYPES[] = {
    "Relay",      /* ACTUATOR_TYPE_RELAY */
    "PWM",        /* ACTUATOR_TYPE_PWM */
    "Latching",   /* ACTUATOR_TYPE_LATCHING */
    "Momentary",  /* ACTUATOR_TYPE_MOMENTARY */
    "Pump",       /* ACTUATOR_TYPE_PUMP */
    "Valve"       /* ACTUATOR_TYPE_VALVE */
};
static const int TYPE_COUNT = 6;

/* Safe state options - maps to safe_state_t enum */
static const char *SAFE_STATES[] = {
    "OFF",   /* SAFE_STATE_OFF */
    "ON",    /* SAFE_STATE_ON */
    "Hold"   /* SAFE_STATE_HOLD */
};
static const int SAFE_STATE_COUNT = 3;

/* ============================================================================
 * Pin Selector Popup
 * ========================================================================== */

typedef struct {
    int pin;
    const char *label;
    bool in_use;
} pin_option_t;

/**
 * Show GPIO pin selector popup with board-specific defaults
 * Returns selected pin or -1 if cancelled
 */
static int show_pin_selector(int current_pin, const char *gpio_chip, int exclude_id) {
    board_info_t board;
    board_detect(&board);

    /* Build list of suggested pins based on board type */
    pin_option_t pins[16];
    int pin_count = 0;

    /* Add board-specific relay/output pins */
    if (board.pins.gpio_relay_1 >= 0) {
        pins[pin_count++] = (pin_option_t){board.pins.gpio_relay_1, "Relay 1", false};
    }
    if (board.pins.gpio_relay_2 >= 0) {
        pins[pin_count++] = (pin_option_t){board.pins.gpio_relay_2, "Relay 2", false};
    }
    if (board.pins.gpio_relay_3 >= 0) {
        pins[pin_count++] = (pin_option_t){board.pins.gpio_relay_3, "Relay 3", false};
    }
    if (board.pins.gpio_relay_4 >= 0) {
        pins[pin_count++] = (pin_option_t){board.pins.gpio_relay_4, "Relay 4", false};
    }

    /* Add PWM pins if available */
    if (board.pins.pwm_channel_0 >= 0) {
        pins[pin_count++] = (pin_option_t){board.pins.pwm_channel_0, "PWM 0", false};
    }
    if (board.pins.pwm_channel_1 >= 0) {
        pins[pin_count++] = (pin_option_t){board.pins.pwm_channel_1, "PWM 1", false};
    }

    /* Add input pins (can be used as outputs too) */
    if (board.pins.gpio_input_1 >= 0) {
        pins[pin_count++] = (pin_option_t){board.pins.gpio_input_1, "GPIO In 1", false};
    }
    if (board.pins.gpio_input_2 >= 0) {
        pins[pin_count++] = (pin_option_t){board.pins.gpio_input_2, "GPIO In 2", false};
    }

    if (pin_count == 0) {
        /* Fallback: common GPIO pins if no board detected */
        pins[pin_count++] = (pin_option_t){17, "GPIO 17", false};
        pins[pin_count++] = (pin_option_t){27, "GPIO 27", false};
        pins[pin_count++] = (pin_option_t){22, "GPIO 22", false};
        pins[pin_count++] = (pin_option_t){23, "GPIO 23", false};
        pins[pin_count++] = (pin_option_t){24, "GPIO 24", false};
        pins[pin_count++] = (pin_option_t){25, "GPIO 25", false};
    }

    /* Check which pins are already in use */
    database_t *db = tui_get_database();
    if (db) {
        for (int i = 0; i < pin_count; i++) {
            gpio_conflict_t conflict;
            if (db_actuator_gpio_conflict_check(db, pins[i].pin, gpio_chip,
                    exclude_id, &conflict) == RESULT_OK) {
                pins[i].in_use = conflict.has_conflict;
            }
        }
    }

    /* Draw popup */
    int dialog_h = pin_count + 6;
    int dialog_w = 40;
    int dialog_y = (LINES - dialog_h) / 2;
    int dialog_x = (COLS - dialog_w) / 2;

    WINDOW *popup = newwin(dialog_h, dialog_w, dialog_y, dialog_x);
    keypad(popup, TRUE);
    box(popup, 0, 0);

    wattron(popup, A_BOLD);
    mvwprintw(popup, 0, (dialog_w - 18) / 2, " Select GPIO Pin ");
    wattroff(popup, A_BOLD);

    /* Board info */
    mvwprintw(popup, 1, 2, "Board: %s", board.name);
    mvwprintw(popup, 2, 2, "Chip: %s", board.pins.gpio_chip);
    mvwhline(popup, 3, 1, ACS_HLINE, dialog_w - 2);

    int selected = 0;
    /* Find current pin in list */
    for (int i = 0; i < pin_count; i++) {
        if (pins[i].pin == current_pin) {
            selected = i;
            break;
        }
    }

    bool running = true;
    while (running) {
        /* Draw pin options */
        for (int i = 0; i < pin_count; i++) {
            if (i == selected) {
                wattron(popup, A_REVERSE);
            }

            if (pins[i].in_use) {
                wattron(popup, COLOR_PAIR(TUI_COLOR_ERROR));
                mvwprintw(popup, 4 + i, 2, " GPIO %2d - %-12s [IN USE] ",
                          pins[i].pin, pins[i].label);
                wattroff(popup, COLOR_PAIR(TUI_COLOR_ERROR));
            } else {
                mvwprintw(popup, 4 + i, 2, " GPIO %2d - %-12s          ",
                          pins[i].pin, pins[i].label);
            }

            if (i == selected) {
                wattroff(popup, A_REVERSE);
            }
        }

        wattron(popup, A_DIM);
        mvwprintw(popup, dialog_h - 2, 2, "Enter: Select | Esc: Cancel");
        wattroff(popup, A_DIM);

        wrefresh(popup);

        int ch = wgetch(popup);
        switch (ch) {
            case KEY_UP:
                if (selected > 0) selected--;
                break;
            case KEY_DOWN:
                if (selected < pin_count - 1) selected++;
                break;
            case '\n':
            case KEY_ENTER:
                if (!pins[selected].in_use) {
                    delwin(popup);
                    return pins[selected].pin;
                }
                break;
            case 27:
                running = false;
                break;
        }
    }

    delwin(popup);
    return -1;
}

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

static bool is_pwm_type(void) {
    return g_dlg.form->type == ACTUATOR_TYPE_PWM ||
           g_dlg.form->type == ACTUATOR_TYPE_PUMP;
}

static void draw_dialog(void) {
    int dialog_h = 18;
    int dialog_w = 60;
    int dialog_y = (LINES - dialog_h) / 2;
    int dialog_x = (COLS - dialog_w) / 2;

    /* Draw dialog box */
    attron(COLOR_PAIR(1));
    for (int y = 0; y < dialog_h; y++) {
        mvhline(dialog_y + y, dialog_x, ' ', dialog_w);
    }

    /* Border */
    mvhline(dialog_y, dialog_x, ACS_HLINE, dialog_w);
    mvhline(dialog_y + dialog_h - 1, dialog_x, ACS_HLINE, dialog_w);
    mvvline(dialog_y, dialog_x, ACS_VLINE, dialog_h);
    mvvline(dialog_y, dialog_x + dialog_w - 1, ACS_VLINE, dialog_h);
    mvaddch(dialog_y, dialog_x, ACS_ULCORNER);
    mvaddch(dialog_y, dialog_x + dialog_w - 1, ACS_URCORNER);
    mvaddch(dialog_y + dialog_h - 1, dialog_x, ACS_LLCORNER);
    mvaddch(dialog_y + dialog_h - 1, dialog_x + dialog_w - 1, ACS_LRCORNER);

    /* Title */
    const char *title = (g_dlg.mode == ACTUATOR_DIALOG_ADD) ?
                        "[ Add Actuator ]" : "[ Edit Actuator ]";
    mvprintw(dialog_y, dialog_x + (dialog_w - strlen(title)) / 2, "%s", title);
    attroff(COLOR_PAIR(1));

    int row = dialog_y + 2;
    int label_x = dialog_x + 2;
    int value_x = dialog_x + 22;

    /* Name */
    if (g_dlg.current_field == FIELD_NAME) attron(A_REVERSE);
    mvprintw(row, label_x, "Name:");
    if (g_dlg.current_field == FIELD_NAME) attroff(A_REVERSE);
    if (g_dlg.editing && g_dlg.current_field == FIELD_NAME) {
        attron(A_UNDERLINE);
        mvprintw(row, value_x, "%-28s", g_dlg.edit_buffer);
        attroff(A_UNDERLINE);
    } else {
        mvprintw(row, value_x, "%-28s", g_dlg.form->name);
    }
    row++;

    /* Slot */
    if (g_dlg.current_field == FIELD_SLOT) attron(A_REVERSE);
    mvprintw(row, label_x, "Slot:");
    if (g_dlg.current_field == FIELD_SLOT) attroff(A_REVERSE);
    if (g_dlg.editing && g_dlg.current_field == FIELD_SLOT) {
        attron(A_UNDERLINE);
        mvprintw(row, value_x, "%-10s", g_dlg.edit_buffer);
        attroff(A_UNDERLINE);
    } else {
        mvprintw(row, value_x, "%d", g_dlg.form->slot);
    }
    row++;

    /* Subslot */
    if (g_dlg.current_field == FIELD_SUBSLOT) attron(A_REVERSE);
    mvprintw(row, label_x, "Subslot:");
    if (g_dlg.current_field == FIELD_SUBSLOT) attroff(A_REVERSE);
    if (g_dlg.editing && g_dlg.current_field == FIELD_SUBSLOT) {
        attron(A_UNDERLINE);
        mvprintw(row, value_x, "%-10s", g_dlg.edit_buffer);
        attroff(A_UNDERLINE);
    } else {
        mvprintw(row, value_x, "%d", g_dlg.form->subslot);
    }
    row++;

    /* Type */
    if (g_dlg.current_field == FIELD_TYPE) attron(A_REVERSE);
    mvprintw(row, label_x, "Type:");
    if (g_dlg.current_field == FIELD_TYPE) attroff(A_REVERSE);
    int type_idx = (int)g_dlg.form->type;
    if (type_idx < 0 || type_idx >= TYPE_COUNT) type_idx = 0;
    mvprintw(row, value_x, "< %-12s >", TYPES[type_idx]);
    row++;

    /* GPIO Pin */
    if (g_dlg.current_field == FIELD_GPIO_PIN) attron(A_REVERSE);
    mvprintw(row, label_x, "GPIO Pin:");
    if (g_dlg.current_field == FIELD_GPIO_PIN) attroff(A_REVERSE);
    if (g_dlg.editing && g_dlg.current_field == FIELD_GPIO_PIN) {
        attron(A_UNDERLINE);
        mvprintw(row, value_x, "%-10s", g_dlg.edit_buffer);
        attroff(A_UNDERLINE);
    } else {
        mvprintw(row, value_x, "%-4d", g_dlg.form->gpio_pin);
        attron(A_DIM);
        printw("  [p=select]");
        attroff(A_DIM);
    }
    row++;

    /* GPIO Chip */
    if (g_dlg.current_field == FIELD_GPIO_CHIP) attron(A_REVERSE);
    mvprintw(row, label_x, "GPIO Chip:");
    if (g_dlg.current_field == FIELD_GPIO_CHIP) attroff(A_REVERSE);
    if (g_dlg.editing && g_dlg.current_field == FIELD_GPIO_CHIP) {
        attron(A_UNDERLINE);
        mvprintw(row, value_x, "%-20s", g_dlg.edit_buffer);
        attroff(A_UNDERLINE);
    } else {
        mvprintw(row, value_x, "%-20s", g_dlg.form->gpio_chip);
    }
    row++;

    /* Active Low */
    if (g_dlg.current_field == FIELD_ACTIVE_LOW) attron(A_REVERSE);
    mvprintw(row, label_x, "Active Low:");
    if (g_dlg.current_field == FIELD_ACTIVE_LOW) attroff(A_REVERSE);
    mvprintw(row, value_x, "[%c] Invert logic", g_dlg.form->active_low ? 'X' : ' ');
    row++;

    /* PWM frequency (only for PWM/Pump types) */
    if (is_pwm_type()) {
        if (g_dlg.current_field == FIELD_PWM_FREQ) attron(A_REVERSE);
        mvprintw(row, label_x, "PWM Freq (Hz):");
        if (g_dlg.current_field == FIELD_PWM_FREQ) attroff(A_REVERSE);
        if (g_dlg.editing && g_dlg.current_field == FIELD_PWM_FREQ) {
            attron(A_UNDERLINE);
            mvprintw(row, value_x, "%-10s", g_dlg.edit_buffer);
            attroff(A_UNDERLINE);
        } else {
            mvprintw(row, value_x, "%d", g_dlg.form->pwm_frequency_hz);
        }
        row++;
    }

    /* Safety settings section */
    row++;
    mvprintw(row, label_x, "--- Safety Settings ---");
    row++;

    /* Safe state */
    if (g_dlg.current_field == FIELD_SAFE_STATE) attron(A_REVERSE);
    mvprintw(row, label_x, "Safe State:");
    if (g_dlg.current_field == FIELD_SAFE_STATE) attroff(A_REVERSE);
    int safe_idx = (int)g_dlg.form->safe_state;
    if (safe_idx < 0 || safe_idx >= SAFE_STATE_COUNT) safe_idx = 0;
    mvprintw(row, value_x, "< %-10s >", SAFE_STATES[safe_idx]);
    row++;

    /* Min on time */
    if (g_dlg.current_field == FIELD_MIN_ON_TIME) attron(A_REVERSE);
    mvprintw(row, label_x, "Min On (ms):");
    if (g_dlg.current_field == FIELD_MIN_ON_TIME) attroff(A_REVERSE);
    if (g_dlg.editing && g_dlg.current_field == FIELD_MIN_ON_TIME) {
        attron(A_UNDERLINE);
        mvprintw(row, value_x, "%-10s", g_dlg.edit_buffer);
        attroff(A_UNDERLINE);
    } else {
        mvprintw(row, value_x, "%d", g_dlg.form->min_on_time_ms);
    }
    row++;

    /* Max on time */
    if (g_dlg.current_field == FIELD_MAX_ON_TIME) attron(A_REVERSE);
    mvprintw(row, label_x, "Max On (ms):");
    if (g_dlg.current_field == FIELD_MAX_ON_TIME) attroff(A_REVERSE);
    if (g_dlg.editing && g_dlg.current_field == FIELD_MAX_ON_TIME) {
        attron(A_UNDERLINE);
        mvprintw(row, value_x, "%-10s", g_dlg.edit_buffer);
        attroff(A_UNDERLINE);
    } else {
        if (g_dlg.form->max_on_time_ms > 0) {
            mvprintw(row, value_x, "%d", g_dlg.form->max_on_time_ms);
        } else {
            mvprintw(row, value_x, "(unlimited)");
        }
    }
    row++;

    /* Enabled */
    if (g_dlg.current_field == FIELD_ENABLED) attron(A_REVERSE);
    mvprintw(row, label_x, "Enabled:");
    if (g_dlg.current_field == FIELD_ENABLED) attroff(A_REVERSE);
    mvprintw(row, value_x, "[%c]", g_dlg.form->enabled ? 'X' : ' ');

    /* Button hints */
    attron(A_DIM);
    mvprintw(dialog_y + dialog_h - 2, dialog_x + 2,
             "Enter: Edit/Toggle | Tab: Next | F10: Save | Esc: Cancel");
    attroff(A_DIM);

    refresh();
}

static void start_edit(void) {
    g_dlg.editing = true;
    switch (g_dlg.current_field) {
        case FIELD_NAME:
            SAFE_STRNCPY(g_dlg.edit_buffer, g_dlg.form->name, sizeof(g_dlg.edit_buffer));
            break;
        case FIELD_SLOT:
            snprintf(g_dlg.edit_buffer, sizeof(g_dlg.edit_buffer), "%d", g_dlg.form->slot);
            break;
        case FIELD_SUBSLOT:
            snprintf(g_dlg.edit_buffer, sizeof(g_dlg.edit_buffer), "%d", g_dlg.form->subslot);
            break;
        case FIELD_GPIO_PIN:
            snprintf(g_dlg.edit_buffer, sizeof(g_dlg.edit_buffer), "%d", g_dlg.form->gpio_pin);
            break;
        case FIELD_GPIO_CHIP:
            SAFE_STRNCPY(g_dlg.edit_buffer, g_dlg.form->gpio_chip, sizeof(g_dlg.edit_buffer));
            break;
        case FIELD_PWM_FREQ:
            snprintf(g_dlg.edit_buffer, sizeof(g_dlg.edit_buffer), "%d", g_dlg.form->pwm_frequency_hz);
            break;
        case FIELD_MIN_ON_TIME:
            snprintf(g_dlg.edit_buffer, sizeof(g_dlg.edit_buffer), "%d", g_dlg.form->min_on_time_ms);
            break;
        case FIELD_MAX_ON_TIME:
            snprintf(g_dlg.edit_buffer, sizeof(g_dlg.edit_buffer), "%d", g_dlg.form->max_on_time_ms);
            break;
        default:
            g_dlg.editing = false;
            break;
    }
}

static void commit_edit(void) {
    if (!g_dlg.editing) return;

    switch (g_dlg.current_field) {
        case FIELD_NAME:
            SAFE_STRNCPY(g_dlg.form->name, g_dlg.edit_buffer, sizeof(g_dlg.form->name));
            break;
        case FIELD_SLOT:
            g_dlg.form->slot = atoi(g_dlg.edit_buffer);
            break;
        case FIELD_SUBSLOT:
            g_dlg.form->subslot = atoi(g_dlg.edit_buffer);
            break;
        case FIELD_GPIO_PIN:
            g_dlg.form->gpio_pin = atoi(g_dlg.edit_buffer);
            break;
        case FIELD_GPIO_CHIP:
            SAFE_STRNCPY(g_dlg.form->gpio_chip, g_dlg.edit_buffer, sizeof(g_dlg.form->gpio_chip));
            break;
        case FIELD_PWM_FREQ:
            g_dlg.form->pwm_frequency_hz = atoi(g_dlg.edit_buffer);
            break;
        case FIELD_MIN_ON_TIME:
            g_dlg.form->min_on_time_ms = atoi(g_dlg.edit_buffer);
            break;
        case FIELD_MAX_ON_TIME:
            g_dlg.form->max_on_time_ms = atoi(g_dlg.edit_buffer);
            break;
    }
    g_dlg.editing = false;
}

static void handle_toggle_or_cycle(void) {
    int idx;

    switch (g_dlg.current_field) {
        case FIELD_TYPE:
            idx = ((int)g_dlg.form->type + 1) % TYPE_COUNT;
            g_dlg.form->type = (actuator_type_t)idx;
            break;

        case FIELD_ACTIVE_LOW:
            g_dlg.form->active_low = !g_dlg.form->active_low;
            break;

        case FIELD_SAFE_STATE:
            idx = ((int)g_dlg.form->safe_state + 1) % SAFE_STATE_COUNT;
            g_dlg.form->safe_state = (safe_state_t)idx;
            break;

        case FIELD_ENABLED:
            g_dlg.form->enabled = !g_dlg.form->enabled;
            break;

        default:
            start_edit();
            break;
    }
}

static int get_next_field(int current, int direction) {
    int next = current + direction;

    /* Skip PWM field if not PWM type */
    if (!is_pwm_type() && next == FIELD_PWM_FREQ) {
        next += direction;
    }

    /* Wrap around */
    if (next < 0) next = FIELD_ENABLED;
    if (next > FIELD_ENABLED) next = 0;

    /* Re-check PWM skip after wrap */
    if (!is_pwm_type() && next == FIELD_PWM_FREQ) {
        next += direction;
        if (next < 0) next = FIELD_ENABLED;
        if (next > FIELD_ENABLED) next = 0;
    }

    return next;
}

static bool handle_input(int ch) {
    if (g_dlg.editing) {
        size_t len = strlen(g_dlg.edit_buffer);

        switch (ch) {
            case '\n':
            case KEY_ENTER:
                commit_edit();
                return true;

            case 27:
                g_dlg.editing = false;
                return true;

            case KEY_BACKSPACE:
            case 127:
            case '\b':
                if (len > 0) {
                    g_dlg.edit_buffer[len - 1] = '\0';
                }
                return true;

            default:
                if (isprint(ch) && len < sizeof(g_dlg.edit_buffer) - 1) {
                    g_dlg.edit_buffer[len] = (char)ch;
                    g_dlg.edit_buffer[len + 1] = '\0';
                }
                return true;
        }
    }

    switch (ch) {
        case 27:
            return false;

        case KEY_F(10):
        case KEY_F(2):
            g_dlg.confirmed = true;
            return false;

        case '\n':
        case KEY_ENTER:
        case ' ':
            handle_toggle_or_cycle();
            break;

        case KEY_UP:
            g_dlg.current_field = get_next_field(g_dlg.current_field, -1);
            break;

        case KEY_DOWN:
        case '\t':
            g_dlg.current_field = get_next_field(g_dlg.current_field, 1);
            break;

        case KEY_LEFT:
            /* Reverse cycle for cycle fields */
            if (g_dlg.current_field == FIELD_TYPE) {
                int idx = ((int)g_dlg.form->type - 1 + TYPE_COUNT) % TYPE_COUNT;
                g_dlg.form->type = (actuator_type_t)idx;
            } else if (g_dlg.current_field == FIELD_SAFE_STATE) {
                int idx = ((int)g_dlg.form->safe_state - 1 + SAFE_STATE_COUNT) % SAFE_STATE_COUNT;
                g_dlg.form->safe_state = (safe_state_t)idx;
            }
            break;

        case KEY_RIGHT:
            handle_toggle_or_cycle();
            break;

        case 'p':
        case 'P':
            /* Open pin selector when on GPIO Pin field */
            if (g_dlg.current_field == FIELD_GPIO_PIN) {
                int new_pin = show_pin_selector(g_dlg.form->gpio_pin,
                                                g_dlg.form->gpio_chip,
                                                g_dlg.form->id);
                if (new_pin >= 0) {
                    g_dlg.form->gpio_pin = new_pin;
                }
            }
            break;
    }

    return true;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

void dialog_actuator_init_form(actuator_form_t *form) {
    memset(form, 0, sizeof(actuator_form_t));
    SAFE_STRNCPY(form->name, "New Actuator", sizeof(form->name));
    form->slot = 9;
    form->subslot = 1;
    form->type = ACTUATOR_TYPE_RELAY;
    form->gpio_pin = 17;
    SAFE_STRNCPY(form->gpio_chip, "gpiochip0", sizeof(form->gpio_chip));
    form->active_low = false;
    form->pwm_frequency_hz = 1000;
    form->safe_state = SAFE_STATE_OFF;
    form->min_on_time_ms = 0;
    form->max_on_time_ms = 0;
    form->enabled = true;
}

void dialog_actuator_load(actuator_form_t *form, const db_actuator_t *actuator) {
    form->id = actuator->id;
    SAFE_STRNCPY(form->name, actuator->name, sizeof(form->name));
    form->slot = actuator->slot;
    form->subslot = actuator->subslot;
    form->type = actuator->type;
    form->gpio_pin = actuator->gpio_pin;
    SAFE_STRNCPY(form->gpio_chip, actuator->gpio_chip, sizeof(form->gpio_chip));
    form->active_low = actuator->active_low;
    form->pwm_frequency_hz = actuator->pwm_frequency_hz;
    form->safe_state = actuator->safe_state;
    form->min_on_time_ms = actuator->min_on_time_ms;
    form->max_on_time_ms = actuator->max_on_time_ms;
    form->enabled = actuator->enabled;
}

void dialog_actuator_save(const actuator_form_t *form, db_actuator_t *actuator) {
    actuator->id = form->id;
    SAFE_STRNCPY(actuator->name, form->name, sizeof(actuator->name));
    actuator->slot = form->slot;
    actuator->subslot = form->subslot;
    actuator->type = form->type;
    actuator->gpio_pin = form->gpio_pin;
    SAFE_STRNCPY(actuator->gpio_chip, form->gpio_chip, sizeof(actuator->gpio_chip));
    actuator->active_low = form->active_low;
    actuator->pwm_frequency_hz = form->pwm_frequency_hz;
    actuator->safe_state = form->safe_state;
    actuator->min_on_time_ms = form->min_on_time_ms;
    actuator->max_on_time_ms = form->max_on_time_ms;
    actuator->enabled = form->enabled;
}

bool dialog_actuator_show(actuator_dialog_mode_t mode, actuator_form_t *form) {
    memset(&g_dlg, 0, sizeof(g_dlg));
    g_dlg.mode = mode;
    g_dlg.form = form;

    bool running = true;
    while (running) {
        draw_dialog();
        int ch = getch();
        running = handle_input(ch);
    }

    return g_dlg.confirmed;
}

bool dialog_actuator_confirm_delete(const char *actuator_name) {
    int dialog_h = 7;
    int dialog_w = 50;
    int dialog_y = (LINES - dialog_h) / 2;
    int dialog_x = (COLS - dialog_w) / 2;

    /* Draw confirmation box */
    attron(COLOR_PAIR(3));  /* Warning color */
    for (int y = 0; y < dialog_h; y++) {
        mvhline(dialog_y + y, dialog_x, ' ', dialog_w);
    }

    /* Border */
    mvhline(dialog_y, dialog_x, ACS_HLINE, dialog_w);
    mvhline(dialog_y + dialog_h - 1, dialog_x, ACS_HLINE, dialog_w);
    mvvline(dialog_y, dialog_x, ACS_VLINE, dialog_h);
    mvvline(dialog_y, dialog_x + dialog_w - 1, ACS_VLINE, dialog_h);
    mvaddch(dialog_y, dialog_x, ACS_ULCORNER);
    mvaddch(dialog_y, dialog_x + dialog_w - 1, ACS_URCORNER);
    mvaddch(dialog_y + dialog_h - 1, dialog_x, ACS_LLCORNER);
    mvaddch(dialog_y + dialog_h - 1, dialog_x + dialog_w - 1, ACS_LRCORNER);

    mvprintw(dialog_y, dialog_x + (dialog_w - 18) / 2, "[ Confirm Delete ]");
    attroff(COLOR_PAIR(3));

    mvprintw(dialog_y + 2, dialog_x + 2, "Delete actuator '%s'?", actuator_name);
    mvprintw(dialog_y + 4, dialog_x + 2, "Press Y to confirm, N or Esc to cancel");

    refresh();

    /* Wait for response */
    while (1) {
        int ch = getch();
        if (ch == 'y' || ch == 'Y') {
            return true;
        }
        if (ch == 'n' || ch == 'N' || ch == 27) {
            return false;
        }
    }
}
