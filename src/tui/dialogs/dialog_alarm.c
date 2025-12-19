/**
 * @file dialog_alarm.c
 * @brief Alarm rule add/edit dialog implementation
 */

#include "dialog_alarm.h"
#include "tui/tui_common.h"
#include "db/db_modules.h"
#include "db/database.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Get database via TUI */

/* Field indices */
enum {
    FIELD_NAME = 0,
    FIELD_MODULE,
    FIELD_CONDITION,
    FIELD_THRESHOLD_HIGH,
    FIELD_THRESHOLD_LOW,
    FIELD_SEVERITY,
    FIELD_HYSTERESIS,
    FIELD_AUTO_CLEAR,
    FIELD_ENABLED,
    FIELD_INTERLOCK_ENABLED,
    FIELD_INTERLOCK_SLOT,
    FIELD_INTERLOCK_ACTION,
    FIELD_INTERLOCK_PWM,
    FIELD_RELEASE_ON_CLEAR,
    FIELD_COUNT
};

/* Dialog state */
static struct {
    alarm_dialog_mode_t mode;
    alarm_form_t *form;
    int current_field;
    char edit_buffer[64];
    bool editing;
    bool confirmed;

    /* Module list for selection */
    db_module_t *modules;
    int module_count;
    int selected_module_idx;
} g_dlg = {0};

/* Condition options - maps to alarm_condition_t enum */
static const char *CONDITIONS[] = {
    "Above High",    /* ALARM_CONDITION_ABOVE_THRESHOLD */
    "Below Low",     /* ALARM_CONDITION_BELOW_THRESHOLD */
    "Out of Range",  /* ALARM_CONDITION_OUT_OF_RANGE */
    "Rate Change",   /* ALARM_CONDITION_RATE_OF_CHANGE */
    "Deviation"      /* ALARM_CONDITION_DEVIATION */
};
static const int CONDITION_COUNT = 5;

/* Severity options - maps to alarm_severity_t enum */
static const char *SEVERITIES[] = {"Low", "Medium", "High", "Critical"};
static const int SEVERITY_COUNT = 4;

/* Interlock action options - maps to interlock_action_t enum */
static const char *INTERLOCK_ACTIONS[] = {"None", "Turn OFF", "Turn ON", "Set PWM"};
static const int INTERLOCK_ACTION_COUNT = 4;

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

static int find_module_index(int module_id) {
    for (int i = 0; i < g_dlg.module_count; i++) {
        if (g_dlg.modules[i].id == module_id) return i;
    }
    return 0;
}

static void load_modules(void) {
    if (g_dlg.modules) {
        free(g_dlg.modules);
        g_dlg.modules = NULL;
    }
    g_dlg.module_count = 0;

    database_t *db = tui_get_database();
    if (db) {
        db_module_list(db, &g_dlg.modules, &g_dlg.module_count);
    }
}

static void draw_dialog(void) {
    int dialog_h = 22;
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
    const char *title = (g_dlg.mode == ALARM_DIALOG_ADD) ?
                        "[ Add Alarm Rule ]" : "[ Edit Alarm Rule ]";
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
        mvprintw(row, value_x, "%-30s", g_dlg.edit_buffer);
        attroff(A_UNDERLINE);
    } else {
        mvprintw(row, value_x, "%-30s", g_dlg.form->name);
    }
    row++;

    /* Module (sensor) */
    if (g_dlg.current_field == FIELD_MODULE) attron(A_REVERSE);
    mvprintw(row, label_x, "Sensor:");
    if (g_dlg.current_field == FIELD_MODULE) attroff(A_REVERSE);
    if (g_dlg.module_count > 0 && g_dlg.selected_module_idx < g_dlg.module_count) {
        mvprintw(row, value_x, "< %-24s >",
                 g_dlg.modules[g_dlg.selected_module_idx].name);
    } else {
        mvprintw(row, value_x, "< No sensors available   >");
    }
    row++;

    /* Condition */
    if (g_dlg.current_field == FIELD_CONDITION) attron(A_REVERSE);
    mvprintw(row, label_x, "Condition:");
    if (g_dlg.current_field == FIELD_CONDITION) attroff(A_REVERSE);
    int cond_idx = (int)g_dlg.form->condition;
    if (cond_idx < 0 || cond_idx >= CONDITION_COUNT) cond_idx = 0;
    mvprintw(row, value_x, "< %s >", CONDITIONS[cond_idx]);
    row++;

    /* Threshold High */
    if (g_dlg.current_field == FIELD_THRESHOLD_HIGH) attron(A_REVERSE);
    mvprintw(row, label_x, "Threshold High:");
    if (g_dlg.current_field == FIELD_THRESHOLD_HIGH) attroff(A_REVERSE);
    if (g_dlg.editing && g_dlg.current_field == FIELD_THRESHOLD_HIGH) {
        attron(A_UNDERLINE);
        mvprintw(row, value_x, "%-20s", g_dlg.edit_buffer);
        attroff(A_UNDERLINE);
    } else {
        mvprintw(row, value_x, "%.2f", g_dlg.form->threshold_high);
    }
    row++;

    /* Threshold Low */
    if (g_dlg.current_field == FIELD_THRESHOLD_LOW) attron(A_REVERSE);
    mvprintw(row, label_x, "Threshold Low:");
    if (g_dlg.current_field == FIELD_THRESHOLD_LOW) attroff(A_REVERSE);
    if (g_dlg.editing && g_dlg.current_field == FIELD_THRESHOLD_LOW) {
        attron(A_UNDERLINE);
        mvprintw(row, value_x, "%-20s", g_dlg.edit_buffer);
        attroff(A_UNDERLINE);
    } else {
        mvprintw(row, value_x, "%.2f", g_dlg.form->threshold_low);
    }
    row++;

    /* Severity */
    if (g_dlg.current_field == FIELD_SEVERITY) attron(A_REVERSE);
    mvprintw(row, label_x, "Severity:");
    if (g_dlg.current_field == FIELD_SEVERITY) attroff(A_REVERSE);
    int sev_idx = (int)g_dlg.form->severity;
    if (sev_idx < 0 || sev_idx >= SEVERITY_COUNT) sev_idx = 0;
    mvprintw(row, value_x, "< %s >", SEVERITIES[sev_idx]);
    row++;

    /* Hysteresis */
    if (g_dlg.current_field == FIELD_HYSTERESIS) attron(A_REVERSE);
    mvprintw(row, label_x, "Hysteresis %%:");
    if (g_dlg.current_field == FIELD_HYSTERESIS) attroff(A_REVERSE);
    if (g_dlg.editing && g_dlg.current_field == FIELD_HYSTERESIS) {
        attron(A_UNDERLINE);
        mvprintw(row, value_x, "%-10s", g_dlg.edit_buffer);
        attroff(A_UNDERLINE);
    } else {
        mvprintw(row, value_x, "%d%%", g_dlg.form->hysteresis_percent);
    }
    row++;

    /* Auto-clear */
    if (g_dlg.current_field == FIELD_AUTO_CLEAR) attron(A_REVERSE);
    mvprintw(row, label_x, "Auto-clear:");
    if (g_dlg.current_field == FIELD_AUTO_CLEAR) attroff(A_REVERSE);
    mvprintw(row, value_x, "[%c]", g_dlg.form->auto_clear ? 'X' : ' ');
    row++;

    /* Enabled */
    if (g_dlg.current_field == FIELD_ENABLED) attron(A_REVERSE);
    mvprintw(row, label_x, "Enabled:");
    if (g_dlg.current_field == FIELD_ENABLED) attroff(A_REVERSE);
    mvprintw(row, value_x, "[%c]", g_dlg.form->enabled ? 'X' : ' ');
    row++;

    /* Separator */
    row++;
    mvprintw(row, label_x, "--- Safety Interlock ---");
    row++;

    /* Interlock enabled */
    if (g_dlg.current_field == FIELD_INTERLOCK_ENABLED) attron(A_REVERSE);
    mvprintw(row, label_x, "Interlock:");
    if (g_dlg.current_field == FIELD_INTERLOCK_ENABLED) attroff(A_REVERSE);
    mvprintw(row, value_x, "[%c] Enable safety interlock",
             g_dlg.form->interlock_enabled ? 'X' : ' ');
    row++;

    if (g_dlg.form->interlock_enabled) {
        /* Interlock slot */
        if (g_dlg.current_field == FIELD_INTERLOCK_SLOT) attron(A_REVERSE);
        mvprintw(row, label_x, "  Actuator Slot:");
        if (g_dlg.current_field == FIELD_INTERLOCK_SLOT) attroff(A_REVERSE);
        if (g_dlg.editing && g_dlg.current_field == FIELD_INTERLOCK_SLOT) {
            attron(A_UNDERLINE);
            mvprintw(row, value_x, "%-10s", g_dlg.edit_buffer);
            attroff(A_UNDERLINE);
        } else {
            mvprintw(row, value_x, "%d", g_dlg.form->interlock_slot);
        }
        row++;

        /* Interlock action */
        if (g_dlg.current_field == FIELD_INTERLOCK_ACTION) attron(A_REVERSE);
        mvprintw(row, label_x, "  Action:");
        if (g_dlg.current_field == FIELD_INTERLOCK_ACTION) attroff(A_REVERSE);
        int act_idx = (int)g_dlg.form->interlock_action;
        if (act_idx < 0 || act_idx >= INTERLOCK_ACTION_COUNT) act_idx = 0;
        mvprintw(row, value_x, "< %s >", INTERLOCK_ACTIONS[act_idx]);
        row++;

        /* PWM duty (only if action is PWM) */
        if (g_dlg.form->interlock_action == INTERLOCK_ACTION_PWM) {
            if (g_dlg.current_field == FIELD_INTERLOCK_PWM) attron(A_REVERSE);
            mvprintw(row, label_x, "  PWM Duty %%:");
            if (g_dlg.current_field == FIELD_INTERLOCK_PWM) attroff(A_REVERSE);
            if (g_dlg.editing && g_dlg.current_field == FIELD_INTERLOCK_PWM) {
                attron(A_UNDERLINE);
                mvprintw(row, value_x, "%-10s", g_dlg.edit_buffer);
                attroff(A_UNDERLINE);
            } else {
                mvprintw(row, value_x, "%d%%", g_dlg.form->interlock_pwm_duty);
            }
            row++;
        }

        /* Release on clear */
        if (g_dlg.current_field == FIELD_RELEASE_ON_CLEAR) attron(A_REVERSE);
        mvprintw(row, label_x, "  Auto-release:");
        if (g_dlg.current_field == FIELD_RELEASE_ON_CLEAR) attroff(A_REVERSE);
        mvprintw(row, value_x, "[%c] Release when alarm clears",
                 g_dlg.form->release_on_clear ? 'X' : ' ');
    }

    /* Button hints */
    attron(A_DIM);
    mvprintw(dialog_y + dialog_h - 2, dialog_x + 2,
             "Enter: Edit/Toggle | Tab/Arrows: Navigate | F10: Save | Esc: Cancel");
    attroff(A_DIM);

    refresh();
}

static void start_edit(void) {
    g_dlg.editing = true;
    switch (g_dlg.current_field) {
        case FIELD_NAME:
            SAFE_STRNCPY(g_dlg.edit_buffer, g_dlg.form->name, sizeof(g_dlg.edit_buffer));
            break;
        case FIELD_THRESHOLD_HIGH:
            snprintf(g_dlg.edit_buffer, sizeof(g_dlg.edit_buffer), "%.2f",
                     g_dlg.form->threshold_high);
            break;
        case FIELD_THRESHOLD_LOW:
            snprintf(g_dlg.edit_buffer, sizeof(g_dlg.edit_buffer), "%.2f",
                     g_dlg.form->threshold_low);
            break;
        case FIELD_HYSTERESIS:
            snprintf(g_dlg.edit_buffer, sizeof(g_dlg.edit_buffer), "%d",
                     g_dlg.form->hysteresis_percent);
            break;
        case FIELD_INTERLOCK_SLOT:
            snprintf(g_dlg.edit_buffer, sizeof(g_dlg.edit_buffer), "%d",
                     g_dlg.form->interlock_slot);
            break;
        case FIELD_INTERLOCK_PWM:
            snprintf(g_dlg.edit_buffer, sizeof(g_dlg.edit_buffer), "%d",
                     g_dlg.form->interlock_pwm_duty);
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
        case FIELD_THRESHOLD_HIGH:
            g_dlg.form->threshold_high = (float)atof(g_dlg.edit_buffer);
            break;
        case FIELD_THRESHOLD_LOW:
            g_dlg.form->threshold_low = (float)atof(g_dlg.edit_buffer);
            break;
        case FIELD_HYSTERESIS:
            g_dlg.form->hysteresis_percent = atoi(g_dlg.edit_buffer);
            break;
        case FIELD_INTERLOCK_SLOT:
            g_dlg.form->interlock_slot = atoi(g_dlg.edit_buffer);
            break;
        case FIELD_INTERLOCK_PWM: {
            int val = atoi(g_dlg.edit_buffer);
            if (val > 100) val = 100;
            if (val < 0) val = 0;
            g_dlg.form->interlock_pwm_duty = (uint8_t)val;
            break;
        }
    }
    g_dlg.editing = false;
}

static void handle_toggle_or_cycle(void) {
    int idx;

    switch (g_dlg.current_field) {
        case FIELD_MODULE:
            if (g_dlg.module_count > 0) {
                g_dlg.selected_module_idx = (g_dlg.selected_module_idx + 1) % g_dlg.module_count;
                g_dlg.form->module_id = g_dlg.modules[g_dlg.selected_module_idx].id;
            }
            break;

        case FIELD_CONDITION:
            idx = ((int)g_dlg.form->condition + 1) % CONDITION_COUNT;
            g_dlg.form->condition = (alarm_condition_t)idx;
            break;

        case FIELD_SEVERITY:
            idx = ((int)g_dlg.form->severity + 1) % SEVERITY_COUNT;
            g_dlg.form->severity = (alarm_severity_t)idx;
            break;

        case FIELD_AUTO_CLEAR:
            g_dlg.form->auto_clear = !g_dlg.form->auto_clear;
            break;

        case FIELD_ENABLED:
            g_dlg.form->enabled = !g_dlg.form->enabled;
            break;

        case FIELD_INTERLOCK_ENABLED:
            g_dlg.form->interlock_enabled = !g_dlg.form->interlock_enabled;
            break;

        case FIELD_INTERLOCK_ACTION:
            idx = ((int)g_dlg.form->interlock_action + 1) % INTERLOCK_ACTION_COUNT;
            g_dlg.form->interlock_action = (interlock_action_t)idx;
            break;

        case FIELD_RELEASE_ON_CLEAR:
            g_dlg.form->release_on_clear = !g_dlg.form->release_on_clear;
            break;

        default:
            start_edit();
            break;
    }
}

static bool handle_input(int ch) {
    if (g_dlg.editing) {
        size_t len = strlen(g_dlg.edit_buffer);

        switch (ch) {
            case '\n':
            case KEY_ENTER:
                commit_edit();
                return true;

            case 27:  /* ESC */
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
        case 27:  /* ESC */
            return false;  /* Cancel */

        case KEY_F(10):
        case KEY_F(2):
            g_dlg.confirmed = true;
            return false;  /* Save and exit */

        case '\n':
        case KEY_ENTER:
        case ' ':
            handle_toggle_or_cycle();
            break;

        case KEY_UP:
            if (g_dlg.current_field > 0) {
                g_dlg.current_field--;
                /* Skip hidden fields */
                if (!g_dlg.form->interlock_enabled &&
                    g_dlg.current_field >= FIELD_INTERLOCK_SLOT) {
                    g_dlg.current_field = FIELD_INTERLOCK_ENABLED;
                }
                if (g_dlg.form->interlock_action != INTERLOCK_ACTION_PWM &&
                    g_dlg.current_field == FIELD_INTERLOCK_PWM) {
                    g_dlg.current_field--;
                }
            }
            break;

        case KEY_DOWN:
        case '\t':
            g_dlg.current_field++;
            /* Skip hidden fields */
            if (!g_dlg.form->interlock_enabled &&
                g_dlg.current_field > FIELD_INTERLOCK_ENABLED) {
                g_dlg.current_field = FIELD_INTERLOCK_ENABLED;
            }
            if (g_dlg.form->interlock_action != INTERLOCK_ACTION_PWM &&
                g_dlg.current_field == FIELD_INTERLOCK_PWM) {
                g_dlg.current_field++;
            }
            /* Wrap around */
            int max_field = g_dlg.form->interlock_enabled ? FIELD_RELEASE_ON_CLEAR : FIELD_INTERLOCK_ENABLED;
            if (g_dlg.current_field > max_field) {
                g_dlg.current_field = 0;
            }
            break;

        case KEY_LEFT:
            /* Cycle backwards for cycle fields */
            if (g_dlg.current_field == FIELD_MODULE && g_dlg.module_count > 0) {
                g_dlg.selected_module_idx = (g_dlg.selected_module_idx - 1 + g_dlg.module_count) % g_dlg.module_count;
                g_dlg.form->module_id = g_dlg.modules[g_dlg.selected_module_idx].id;
            } else if (g_dlg.current_field == FIELD_CONDITION) {
                int idx = ((int)g_dlg.form->condition - 1 + CONDITION_COUNT) % CONDITION_COUNT;
                g_dlg.form->condition = (alarm_condition_t)idx;
            } else if (g_dlg.current_field == FIELD_SEVERITY) {
                int idx = ((int)g_dlg.form->severity - 1 + SEVERITY_COUNT) % SEVERITY_COUNT;
                g_dlg.form->severity = (alarm_severity_t)idx;
            } else if (g_dlg.current_field == FIELD_INTERLOCK_ACTION) {
                int idx = ((int)g_dlg.form->interlock_action - 1 + INTERLOCK_ACTION_COUNT) % INTERLOCK_ACTION_COUNT;
                g_dlg.form->interlock_action = (interlock_action_t)idx;
            }
            break;

        case KEY_RIGHT:
            handle_toggle_or_cycle();
            break;
    }

    return true;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

void dialog_alarm_init_form(alarm_form_t *form) {
    memset(form, 0, sizeof(alarm_form_t));
    SAFE_STRNCPY(form->name, "New Alarm", sizeof(form->name));
    form->condition = ALARM_CONDITION_ABOVE_THRESHOLD;
    form->threshold_high = 100.0f;
    form->threshold_low = 0.0f;
    form->severity = ALARM_SEVERITY_MEDIUM;
    form->hysteresis_percent = 5;
    form->enabled = true;
    form->auto_clear = true;
    form->interlock_enabled = false;
    form->interlock_slot = 9;
    form->interlock_action = INTERLOCK_ACTION_OFF;
    form->interlock_pwm_duty = 0;
    form->release_on_clear = true;
}

void dialog_alarm_load_rule(alarm_form_t *form, const db_alarm_rule_t *rule) {
    form->rule_id = rule->id;
    SAFE_STRNCPY(form->name, rule->name, sizeof(form->name));
    form->module_id = rule->module_id;
    form->condition = rule->condition;
    form->threshold_high = rule->threshold_high;
    form->threshold_low = rule->threshold_low;
    form->severity = rule->severity;
    form->hysteresis_percent = rule->hysteresis_percent;
    form->enabled = rule->enabled;
    form->auto_clear = rule->auto_clear;
    form->interlock_enabled = rule->interlock_enabled;
    form->interlock_slot = rule->interlock_slot;
    form->interlock_action = rule->interlock_action;
    form->interlock_pwm_duty = rule->interlock_pwm_duty;
    form->release_on_clear = rule->release_on_clear;
}

void dialog_alarm_save_to_rule(const alarm_form_t *form, db_alarm_rule_t *rule) {
    rule->id = form->rule_id;
    SAFE_STRNCPY(rule->name, form->name, sizeof(rule->name));
    rule->module_id = form->module_id;
    rule->condition = form->condition;
    rule->threshold_high = form->threshold_high;
    rule->threshold_low = form->threshold_low;
    rule->severity = form->severity;
    rule->hysteresis_percent = form->hysteresis_percent;
    rule->enabled = form->enabled;
    rule->auto_clear = form->auto_clear;
    rule->interlock_enabled = form->interlock_enabled;
    rule->interlock_slot = form->interlock_slot;
    rule->interlock_action = form->interlock_action;
    rule->interlock_pwm_duty = form->interlock_pwm_duty;
    rule->release_on_clear = form->release_on_clear;
}

bool dialog_alarm_show(alarm_dialog_mode_t mode, alarm_form_t *form) {
    memset(&g_dlg, 0, sizeof(g_dlg));
    g_dlg.mode = mode;
    g_dlg.form = form;

    /* Load available modules */
    load_modules();

    /* Find selected module index */
    if (form->module_id > 0) {
        g_dlg.selected_module_idx = find_module_index(form->module_id);
    } else if (g_dlg.module_count > 0) {
        form->module_id = g_dlg.modules[0].id;
    }

    bool running = true;
    while (running) {
        draw_dialog();
        int ch = getch();
        running = handle_input(ch);
    }

    /* Cleanup */
    if (g_dlg.modules) {
        free(g_dlg.modules);
    }

    return g_dlg.confirmed;
}
