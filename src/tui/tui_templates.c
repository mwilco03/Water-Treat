/**
 * @file tui_templates.c
 * @brief Implementation of reusable TUI templates
 */

#include "tui_templates.h"
#include "tui_common.h"
#include "dialogs/dialog_helpers.h"
#include "config/config.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ============================================================================
 * List Page Template Implementation
 * ========================================================================== */

void tui_list_page_init(tui_list_page_state_t *state,
                        const tui_list_page_config_t *config,
                        WINDOW *win) {
    memset(state, 0, sizeof(*state));
    state->config = config;
    state->win = win;
    state->needs_reload = true;

    int visible = config->visible_rows > 0 ? config->visible_rows : 15;
    tui_list_init(&state->list, visible);
}

void tui_list_page_load(tui_list_page_state_t *state) {
    if (!state->config || !state->config->load_data) {
        return;
    }

    /* Free existing data */
    if (state->items && state->config->free_data) {
        state->config->free_data(state->items);
        state->items = NULL;
    }

    /* Load new data */
    int count = 0;
    state->config->load_data(&state->items, &count);
    state->item_count = count;

    tui_list_set_count(&state->list, count);
    state->needs_reload = false;
}

void tui_list_page_draw(tui_list_page_state_t *state) {
    if (!state->win || !state->config) {
        return;
    }

    WINDOW *win = state->win;
    const tui_list_page_config_t *cfg = state->config;

    int max_y, max_x;
    getmaxyx(win, max_y, max_x);

    int row = 3;

    /* Draw header row */
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    int col_x = 2;
    for (int c = 0; c < cfg->column_count; c++) {
        int width = cfg->columns[c].width > 0 ? cfg->columns[c].width : 12;
        mvwprintw(win, row, col_x, "%-*s", width, cfg->columns[c].header);
        col_x += width + 1;
    }
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));

    row++;
    mvwhline(win, row++, 2, ACS_HLINE, max_x - 4);

    /* Empty list message */
    if (state->item_count == 0) {
        wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
        mvwprintw(win, row + 2, 4, "No items. Press 'a' to add.");
        wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
        goto draw_help;
    }

    /* Draw items */
    int visible = tui_list_visible_count(&state->list);
    char cell_buf[256];

    for (int i = 0; i < visible; i++) {
        int idx = state->list.scroll_offset + i;
        void *item = (char *)state->items + (idx * cfg->item_size);

        if (idx == state->list.selected) {
            wattron(win, A_REVERSE);
        }

        col_x = 2;
        for (int c = 0; c < cfg->column_count; c++) {
            int width = cfg->columns[c].width > 0 ? cfg->columns[c].width : 12;

            /* Format cell */
            cell_buf[0] = '\0';
            if (cfg->format_cell) {
                cfg->format_cell(item, c, cell_buf, sizeof(cell_buf));
            }

            /* Get color */
            int color = TUI_COLOR_NORMAL;
            if (cfg->get_cell_color) {
                color = cfg->get_cell_color(item, c);
            }

            /* Draw cell */
            if (color != TUI_COLOR_NORMAL) {
                wattron(win, COLOR_PAIR(color));
            }

            if (cfg->columns[c].right_align) {
                mvwprintw(win, row, col_x, "%*s", width, cell_buf);
            } else {
                mvwprintw(win, row, col_x, "%-*s", width, cell_buf);
            }

            if (color != TUI_COLOR_NORMAL) {
                wattroff(win, COLOR_PAIR(color));
            }

            col_x += width + 1;
        }

        if (idx == state->list.selected) {
            wattroff(win, A_REVERSE);
        }

        row++;
    }

    /* Scroll indicator */
    if (state->item_count > cfg->visible_rows) {
        mvwprintw(win, 3, max_x - 8, "[%3d%%]", tui_list_scroll_percent(&state->list));
    }

draw_help:
    /* Draw help line */
    if (cfg->help_text) {
        int help_row = max_y - 4;
        wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
        mvwhline(win, help_row++, 2, ACS_HLINE, max_x - 4);
        mvwprintw(win, help_row, 2, "%s", cfg->help_text);
        wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
    }
}

bool tui_list_page_input(tui_list_page_state_t *state, int ch) {
    if (!state->config) {
        return false;
    }

    /* Let list widget handle navigation */
    if (tui_list_input(&state->list, ch)) {
        return true;
    }

    void *selected = tui_list_page_selected(state);
    int selected_idx = tui_list_page_selected_index(state);

    switch (ch) {
        case '\n':
        case KEY_ENTER:
            if (selected && state->config->on_view) {
                state->config->on_view(selected, selected_idx);
                return true;
            }
            break;

        case 'a':
        case 'A':
            if (state->config->on_add) {
                state->config->on_add(NULL, -1);
                state->needs_reload = true;
                return true;
            }
            break;

        case 'e':
        case 'E':
            if (selected && state->config->on_edit) {
                state->config->on_edit(selected, selected_idx);
                state->needs_reload = true;
                return true;
            }
            break;

        case 'd':
        case 'D':
            if (selected && state->config->on_delete) {
                state->config->on_delete(selected, selected_idx);
                state->needs_reload = true;
                return true;
            }
            break;

        case 'r':
        case 'R':
            if (state->config->on_refresh) {
                state->config->on_refresh(NULL, -1);
            }
            tui_list_page_load(state);
            return true;
    }

    return false;
}

void tui_list_page_cleanup(tui_list_page_state_t *state) {
    if (state->items && state->config && state->config->free_data) {
        state->config->free_data(state->items);
    }
    state->items = NULL;
    state->win = NULL;
}

void *tui_list_page_selected(tui_list_page_state_t *state) {
    if (!state->items || state->list.selected >= state->item_count) {
        return NULL;
    }
    return (char *)state->items + (state->list.selected * state->config->item_size);
}

int tui_list_page_selected_index(tui_list_page_state_t *state) {
    if (state->list.selected >= state->item_count) {
        return -1;
    }
    return state->list.selected;
}

/* ============================================================================
 * Form Dialog Template Implementation
 * ========================================================================== */

void tui_form_init(tui_form_state_t *state, const tui_form_config_t *config) {
    memset(state, 0, sizeof(*state));
    state->config = config;
    state->current_field = 0;

    /* Initialize values with defaults */
    for (int i = 0; i < config->field_count; i++) {
        memset(&state->values[i], 0, sizeof(state->values[i]));
    }
}

void tui_form_draw(tui_form_state_t *state) {
    if (!state->win || !state->config) {
        return;
    }

    WINDOW *win = state->win;
    const tui_form_config_t *cfg = state->config;

    int row = 2;

    for (int i = 0; i < cfg->field_count; i++) {
        const tui_form_field_def_t *field = &cfg->fields[i];

        /* Separator */
        if (field->type == TUI_FIELD_SEPARATOR) {
            mvwhline(win, row++, 2, ACS_HLINE, getmaxx(win) - 4);
            continue;
        }

        /* Highlight current field */
        if (i == state->current_field) {
            wattron(win, A_REVERSE);
        }

        /* Label */
        mvwprintw(win, row, 2, "%-16s: ", field->label);

        /* Value */
        switch (field->type) {
            case TUI_FIELD_TEXT:
            case TUI_FIELD_READONLY:
                wprintw(win, "%-30s", state->values[i].text);
                break;

            case TUI_FIELD_NUMBER:
                wprintw(win, "%-30d", state->values[i].number);
                break;

            case TUI_FIELD_BOOL:
                wprintw(win, "%-30s", state->values[i].boolean ? "Yes" : "No");
                break;

            case TUI_FIELD_SELECT:
                if (field->options && state->values[i].selected < field->option_count) {
                    wprintw(win, "%-30s", field->options[state->values[i].selected]);
                }
                break;

            default:
                break;
        }

        if (i == state->current_field) {
            wattroff(win, A_REVERSE);
        }

        row++;
    }

    /* Status message */
    if (state->status_msg[0] != '\0') {
        mvwprintw(win, row + 1, 2, "%s", state->status_msg);
    }

    /* Help */
    int max_y = getmaxy(win);
    mvwprintw(win, max_y - 2, 2, "Enter:Edit  Tab:Next  Esc:Cancel  F10:Save");
}

int tui_form_input(tui_form_state_t *state, int ch) {
    if (!state->config) {
        return -1;
    }

    const tui_form_config_t *cfg = state->config;
    tui_form_field_def_t *field = (tui_form_field_def_t *)&cfg->fields[state->current_field];

    /* Skip readonly and separator fields */
    while (field->type == TUI_FIELD_READONLY || field->type == TUI_FIELD_SEPARATOR) {
        if (ch == KEY_UP) {
            if (state->current_field > 0) state->current_field--;
            else break;
        } else {
            if (state->current_field < cfg->field_count - 1) state->current_field++;
            else break;
        }
        field = (tui_form_field_def_t *)&cfg->fields[state->current_field];
    }

    switch (ch) {
        case 27:  /* Escape */
            return -1;

        case KEY_F(10):
        case KEY_F(2):
            /* Validate and submit */
            if (cfg->validate) {
                if (!cfg->validate(state->values, cfg->field_count,
                                   state->status_msg, sizeof(state->status_msg))) {
                    return 0;
                }
            }
            if (cfg->on_submit) {
                if (cfg->on_submit(state->values, cfg->field_count, cfg->user_data)) {
                    return 1;
                }
            }
            return 0;

        case KEY_UP:
            if (state->current_field > 0) {
                state->current_field--;
            }
            return 0;

        case KEY_DOWN:
        case '\t':
            if (state->current_field < cfg->field_count - 1) {
                state->current_field++;
            }
            return 0;

        case '\n':
        case KEY_ENTER:
            /* Edit current field */
            switch (field->type) {
                case TUI_FIELD_TEXT: {
                    char buf[256];
                    SAFE_STRNCPY(buf, state->values[state->current_field].text, sizeof(buf));
                    if (dialog_input_string(field->label, "Enter value:", buf, sizeof(buf))) {
                        SAFE_STRNCPY(state->values[state->current_field].text, buf,
                                     sizeof(state->values[state->current_field].text));
                    }
                    break;
                }

                case TUI_FIELD_NUMBER: {
                    int val = state->values[state->current_field].number;
                    if (dialog_input_int(field->label, "Enter value:", &val, -999999, 999999)) {
                        state->values[state->current_field].number = val;
                    }
                    break;
                }

                case TUI_FIELD_BOOL:
                    state->values[state->current_field].boolean =
                        !state->values[state->current_field].boolean;
                    break;

                case TUI_FIELD_SELECT:
                    if (field->options && field->option_count > 0) {
                        int sel = state->values[state->current_field].selected;
                        int result = dialog_select(field->label, field->options,
                                                   field->option_count, sel);
                        if (result >= 0) {
                            state->values[state->current_field].selected = result;
                        }
                    }
                    break;

                default:
                    break;
            }
            return 0;

        case ' ':
            /* Quick toggle for bool */
            if (field->type == TUI_FIELD_BOOL) {
                state->values[state->current_field].boolean =
                    !state->values[state->current_field].boolean;
            }
            return 0;
    }

    return 0;
}

bool tui_form_dialog_show(const tui_form_config_t *config,
                          tui_field_value_t *initial_values) {
    /* Calculate dialog size */
    int height = config->height > 0 ? config->height : config->field_count + 8;
    int width = config->width > 0 ? config->width : 60;

    WINDOW *win = dialog_create(height, width, config->title);
    if (!win) {
        return false;
    }

    tui_form_state_t state;
    tui_form_init(&state, config);
    state.win = win;

    /* Copy initial values if provided */
    if (initial_values) {
        memcpy(state.values, initial_values, sizeof(tui_field_value_t) * config->field_count);
    }

    bool result = false;
    int ch;

    while (true) {
        werase(win);
        box(win, 0, 0);

        wattron(win, A_BOLD);
        mvwprintw(win, 0, (width - strlen(config->title)) / 2, " %s ", config->title);
        wattroff(win, A_BOLD);

        tui_form_draw(&state);
        wrefresh(win);

        ch = wgetch(win);
        int r = tui_form_input(&state, ch);

        if (r == 1) {
            result = true;
            break;
        } else if (r == -1) {
            result = false;
            break;
        }
    }

    delwin(win);
    return result;
}

/* ============================================================================
 * Config Page Template Implementation
 * ========================================================================== */

void tui_config_page_init(tui_config_page_state_t *state,
                          const tui_config_page_config_t *config,
                          WINDOW *win) {
    memset(state, 0, sizeof(*state));
    state->config = config;
    state->win = win;
    state->current_field = 0;
}

void tui_config_page_load(tui_config_page_state_t *state) {
    if (!state->config) {
        return;
    }

    config_manager_t *mgr = tui_get_config_manager();
    if (!mgr) {
        return;
    }

    for (int i = 0; i < state->config->field_count; i++) {
        const tui_config_field_def_t *field = &state->config->fields[i];

        /* Get value from config manager */
        if (config_get_string(mgr, field->section, field->key,
                              state->values[i], sizeof(state->values[i])) != RESULT_OK) {
            /* Use default if not found */
            if (field->default_value) {
                SAFE_STRNCPY(state->values[i], field->default_value, sizeof(state->values[i]));
            } else {
                state->values[i][0] = '\0';
            }
        }

        /* Save original for undo */
        SAFE_STRNCPY(state->original[i], state->values[i], sizeof(state->original[i]));
    }

    state->modified = false;
}

void tui_config_page_draw(tui_config_page_state_t *state) {
    if (!state->win || !state->config) {
        return;
    }

    WINDOW *win = state->win;
    const tui_config_page_config_t *cfg = state->config;

    int max_y, max_x;
    getmaxyx(win, max_y, max_x);

    int row = 3;

    for (int i = 0; i < cfg->field_count; i++) {
        const tui_config_field_def_t *field = &cfg->fields[i];

        /* Highlight current field */
        if (i == state->current_field && !field->readonly) {
            wattron(win, A_REVERSE);
        }

        /* Label */
        mvwprintw(win, row, 2, "%-20s: ", field->label);

        /* Value */
        if (field->readonly) {
            wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
        }
        wprintw(win, "%-40s", state->values[i]);
        if (field->readonly) {
            wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
        }

        /* Modified indicator */
        if (strcmp(state->values[i], state->original[i]) != 0) {
            wprintw(win, " *");
        }

        if (i == state->current_field && !field->readonly) {
            wattroff(win, A_REVERSE);
        }

        row++;
    }

    /* Help line */
    if (cfg->help_text) {
        int help_row = max_y - 4;
        wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
        mvwhline(win, help_row++, 2, ACS_HLINE, max_x - 4);
        mvwprintw(win, help_row, 2, "%s", cfg->help_text);
        wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
    }

    /* Modified status */
    if (state->modified) {
        mvwprintw(win, max_y - 2, max_x - 12, "[Modified]");
    }
}

bool tui_config_page_input(tui_config_page_state_t *state, int ch) {
    if (!state->config) {
        return false;
    }

    const tui_config_page_config_t *cfg = state->config;

    switch (ch) {
        case KEY_UP:
            if (state->current_field > 0) {
                state->current_field--;
                /* Skip readonly fields */
                while (state->current_field > 0 &&
                       cfg->fields[state->current_field].readonly) {
                    state->current_field--;
                }
            }
            return true;

        case KEY_DOWN:
            if (state->current_field < cfg->field_count - 1) {
                state->current_field++;
                /* Skip readonly fields */
                while (state->current_field < cfg->field_count - 1 &&
                       cfg->fields[state->current_field].readonly) {
                    state->current_field++;
                }
            }
            return true;

        case '\n':
        case KEY_ENTER: {
            const tui_config_field_def_t *field = &cfg->fields[state->current_field];
            if (field->readonly) {
                return true;
            }

            char buf[256];
            SAFE_STRNCPY(buf, state->values[state->current_field], sizeof(buf));

            if (field->type == TUI_FIELD_SELECT && field->options) {
                /* Show selection dialog */
                int sel = 0;
                for (int i = 0; i < field->option_count; i++) {
                    if (strcmp(buf, field->options[i]) == 0) {
                        sel = i;
                        break;
                    }
                }
                int result = dialog_select(field->label, field->options, field->option_count, sel);
                if (result >= 0) {
                    SAFE_STRNCPY(state->values[state->current_field],
                                 field->options[result],
                                 sizeof(state->values[state->current_field]));
                    state->modified = true;
                }
            } else {
                /* Show input dialog */
                if (dialog_input_string(field->label, "Enter value:", buf, sizeof(buf))) {
                    SAFE_STRNCPY(state->values[state->current_field], buf,
                                 sizeof(state->values[state->current_field]));
                    state->modified = true;
                }
            }
            return true;
        }

        case 's':
        case 'S':
            if (state->modified) {
                tui_config_page_save(state);
            }
            return true;

        case 'u':
        case 'U':
            /* Undo changes */
            for (int i = 0; i < cfg->field_count; i++) {
                SAFE_STRNCPY(state->values[i], state->original[i], sizeof(state->values[i]));
            }
            state->modified = false;
            tui_set_status("Changes reverted");
            return true;
    }

    return false;
}

bool tui_config_page_save(tui_config_page_state_t *state) {
    if (!state->config) {
        return false;
    }

    config_manager_t *mgr = tui_get_config_manager();
    if (!mgr) {
        return false;
    }

    for (int i = 0; i < state->config->field_count; i++) {
        const tui_config_field_def_t *field = &state->config->fields[i];
        if (field->readonly) {
            continue;
        }

        config_set_string(mgr, field->section, field->key, state->values[i]);
        SAFE_STRNCPY(state->original[i], state->values[i], sizeof(state->original[i]));
    }

    /* Save to file */
    if (config_save_file(mgr, NULL) == RESULT_OK) {
        state->modified = false;
        tui_set_status("Configuration saved");
        return true;
    }

    tui_set_status("Error saving configuration");
    return false;
}

bool tui_config_page_modified(tui_config_page_state_t *state) {
    return state->modified;
}
