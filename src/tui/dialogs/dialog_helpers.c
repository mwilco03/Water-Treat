/**
 * @file dialog_helpers.c
 * @brief Common dialog helper functions
 */

#include "dialog_helpers.h"
#include "../tui_common.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * Dialog Window Management
 * ========================================================================== */

WINDOW* dialog_create(int height, int width, const char *title) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    int start_y = (max_y - height) / 2;
    int start_x = (max_x - width) / 2;
    
    if (start_y < 0) start_y = 0;
    if (start_x < 0) start_x = 0;
    
    WINDOW *dialog = newwin(height, width, start_y, start_x);
    if (!dialog) return NULL;
    
    keypad(dialog, TRUE);
    box(dialog, 0, 0);
    
    if (title && strlen(title) > 0) {
        wattron(dialog, A_BOLD);
        mvwprintw(dialog, 0, (width - strlen(title) - 2) / 2, " %s ", title);
        wattroff(dialog, A_BOLD);
    }
    
    return dialog;
}

WINDOW* dialog_create_at(int y, int x, int height, int width, const char *title) {
    WINDOW *dialog = newwin(height, width, y, x);
    if (!dialog) return NULL;
    
    keypad(dialog, TRUE);
    box(dialog, 0, 0);
    
    if (title && strlen(title) > 0) {
        wattron(dialog, A_BOLD);
        mvwprintw(dialog, 0, (width - strlen(title) - 2) / 2, " %s ", title);
        wattroff(dialog, A_BOLD);
    }
    
    return dialog;
}

void dialog_destroy(WINDOW *dialog) {
    if (dialog) {
        werase(dialog);
        wrefresh(dialog);
        delwin(dialog);
    }
}

void dialog_draw_button(WINDOW *dialog, int y, int x, const char *label, bool selected) {
    if (selected) {
        wattron(dialog, A_REVERSE);
    }
    mvwprintw(dialog, y, x, "[ %s ]", label);
    if (selected) {
        wattroff(dialog, A_REVERSE);
    }
}

void dialog_draw_buttons(WINDOW *dialog, int y, const char **labels, int count, int selected) {
    int width = getmaxx(dialog);
    
    // Calculate total button width
    int total_width = 0;
    for (int i = 0; i < count; i++) {
        total_width += strlen(labels[i]) + 4;  // "[ label ]"
        if (i < count - 1) total_width += 2;   // spacing
    }
    
    int x = (width - total_width) / 2;
    
    for (int i = 0; i < count; i++) {
        dialog_draw_button(dialog, y, x, labels[i], i == selected);
        x += strlen(labels[i]) + 6;
    }
}

/* ============================================================================
 * Message Dialogs
 * ========================================================================== */

void dialog_message(const char *title, const char *message) {
    int msg_len = strlen(message);
    int width = MAX(msg_len + 6, strlen(title) + 6);
    width = MIN(width, 70);
    
    WINDOW *dialog = dialog_create(7, width, title);
    if (!dialog) return;
    
    mvwprintw(dialog, 2, (width - msg_len) / 2, "%s", message);
    
    const char *buttons[] = {"OK"};
    dialog_draw_buttons(dialog, 4, buttons, 1, 0);
    
    wrefresh(dialog);
    wgetch(dialog);
    dialog_destroy(dialog);
}

void dialog_error(const char *message) {
    int msg_len = strlen(message);
    int width = MAX(msg_len + 6, 20);
    width = MIN(width, 70);
    
    WINDOW *dialog = dialog_create(7, width, "Error");
    if (!dialog) return;
    
    wattron(dialog, COLOR_PAIR(TUI_COLOR_ERROR));
    mvwprintw(dialog, 2, (width - msg_len) / 2, "%s", message);
    wattroff(dialog, COLOR_PAIR(TUI_COLOR_ERROR));
    
    const char *buttons[] = {"OK"};
    dialog_draw_buttons(dialog, 4, buttons, 1, 0);
    
    wrefresh(dialog);
    wgetch(dialog);
    dialog_destroy(dialog);
}

void dialog_warning(const char *message) {
    int msg_len = strlen(message);
    int width = MAX(msg_len + 6, 20);
    width = MIN(width, 70);
    
    WINDOW *dialog = dialog_create(7, width, "Warning");
    if (!dialog) return;
    
    wattron(dialog, COLOR_PAIR(TUI_COLOR_WARNING));
    mvwprintw(dialog, 2, (width - msg_len) / 2, "%s", message);
    wattroff(dialog, COLOR_PAIR(TUI_COLOR_WARNING));
    
    const char *buttons[] = {"OK"};
    dialog_draw_buttons(dialog, 4, buttons, 1, 0);
    
    wrefresh(dialog);
    wgetch(dialog);
    dialog_destroy(dialog);
}

void dialog_info(const char *message) {
    int msg_len = strlen(message);
    int width = MAX(msg_len + 6, 20);
    width = MIN(width, 70);
    
    WINDOW *dialog = dialog_create(7, width, "Information");
    if (!dialog) return;
    
    wattron(dialog, COLOR_PAIR(TUI_COLOR_STATUS));
    mvwprintw(dialog, 2, (width - msg_len) / 2, "%s", message);
    wattroff(dialog, COLOR_PAIR(TUI_COLOR_STATUS));
    
    const char *buttons[] = {"OK"};
    dialog_draw_buttons(dialog, 4, buttons, 1, 0);
    
    wrefresh(dialog);
    wgetch(dialog);
    dialog_destroy(dialog);
}

bool dialog_confirm(const char *title, const char *message) {
    int msg_len = strlen(message);
    int width = MAX(msg_len + 6, strlen(title) + 6);
    width = MAX(width, 30);
    width = MIN(width, 70);
    
    WINDOW *dialog = dialog_create(8, width, title);
    if (!dialog) return false;
    
    mvwprintw(dialog, 2, (width - msg_len) / 2, "%s", message);
    
    const char *buttons[] = {"Yes", "No"};
    int selected = 1;  // Default to No
    
    while (1) {
        dialog_draw_buttons(dialog, 5, buttons, 2, selected);
        wrefresh(dialog);
        
        int ch = wgetch(dialog);
        switch (ch) {
            case KEY_LEFT:
            case 'y':
            case 'Y':
                selected = 0;
                break;
            case KEY_RIGHT:
            case 'n':
            case 'N':
                selected = 1;
                break;
            case '\n':
            case KEY_ENTER:
                dialog_destroy(dialog);
                return (selected == 0);
            case 27:  // Escape
                dialog_destroy(dialog);
                return false;
            case '\t':
                selected = (selected + 1) % 2;
                break;
        }
    }
}

dialog_result_t dialog_yes_no_cancel(const char *title, const char *message) {
    int msg_len = strlen(message);
    int width = MAX(msg_len + 6, strlen(title) + 6);
    width = MAX(width, 40);
    width = MIN(width, 70);
    
    WINDOW *dialog = dialog_create(8, width, title);
    if (!dialog) return DIALOG_CANCEL;
    
    mvwprintw(dialog, 2, (width - msg_len) / 2, "%s", message);
    
    const char *buttons[] = {"Yes", "No", "Cancel"};
    int selected = 2;  // Default to Cancel
    
    while (1) {
        dialog_draw_buttons(dialog, 5, buttons, 3, selected);
        wrefresh(dialog);
        
        int ch = wgetch(dialog);
        switch (ch) {
            case KEY_LEFT:
                if (selected > 0) selected--;
                break;
            case KEY_RIGHT:
                if (selected < 2) selected++;
                break;
            case 'y':
            case 'Y':
                dialog_destroy(dialog);
                return DIALOG_YES;
            case 'n':
            case 'N':
                dialog_destroy(dialog);
                return DIALOG_NO;
            case '\n':
            case KEY_ENTER:
                dialog_destroy(dialog);
                switch (selected) {
                    case 0: return DIALOG_YES;
                    case 1: return DIALOG_NO;
                    default: return DIALOG_CANCEL;
                }
            case 27:  // Escape
                dialog_destroy(dialog);
                return DIALOG_CANCEL;
            case '\t':
                selected = (selected + 1) % 3;
                break;
        }
    }
}

/* ============================================================================
 * Input Dialogs
 * ========================================================================== */

bool dialog_input_string(const char *title, const char *prompt, char *buffer, int max_len) {
    int width = MAX(max_len + 6, strlen(title) + 6);
    width = MAX(width, strlen(prompt) + 10);
    width = MIN(width, 70);
    
    WINDOW *dialog = dialog_create(8, width, title);
    if (!dialog) return false;
    
    mvwprintw(dialog, 2, 2, "%s:", prompt);
    
    // Input field
    int input_y = 3;
    int input_x = 2;
    int input_width = width - 4;
    
    mvwhline(dialog, input_y, input_x, ' ', input_width);
    wattron(dialog, COLOR_PAIR(TUI_COLOR_INPUT));
    mvwhline(dialog, input_y, input_x, ' ', input_width);
    wattroff(dialog, COLOR_PAIR(TUI_COLOR_INPUT));
    
    const char *buttons[] = {"OK", "Cancel"};
    int button_selected = 0;
    bool editing = true;
    int pos = strlen(buffer);
    
    curs_set(1);
    
    while (1) {
        // Draw input field
        wattron(dialog, COLOR_PAIR(TUI_COLOR_INPUT));
        mvwhline(dialog, input_y, input_x, ' ', input_width);
        mvwprintw(dialog, input_y, input_x, "%s", buffer);
        wattroff(dialog, COLOR_PAIR(TUI_COLOR_INPUT));
        
        // Draw buttons
        dialog_draw_buttons(dialog, 5, buttons, 2, editing ? -1 : button_selected);
        
        if (editing) {
            wmove(dialog, input_y, input_x + pos);
        }
        wrefresh(dialog);
        
        int ch = wgetch(dialog);
        
        if (editing) {
            switch (ch) {
                case '\n':
                case KEY_ENTER:
                    curs_set(0);
                    dialog_destroy(dialog);
                    return true;
                case 27:  // Escape
                    curs_set(0);
                    dialog_destroy(dialog);
                    return false;
                case '\t':
                    editing = false;
                    curs_set(0);
                    break;
                case KEY_BACKSPACE:
                case 127:
                    if (pos > 0) {
                        memmove(&buffer[pos - 1], &buffer[pos], strlen(buffer) - pos + 1);
                        pos--;
                    }
                    break;
                case KEY_LEFT:
                    if (pos > 0) pos--;
                    break;
                case KEY_RIGHT:
                    if (pos < (int)strlen(buffer)) pos++;
                    break;
                default:
                    if (ch >= 32 && ch < 127 && (int)strlen(buffer) < max_len - 1) {
                        memmove(&buffer[pos + 1], &buffer[pos], strlen(buffer) - pos + 1);
                        buffer[pos++] = ch;
                    }
                    break;
            }
        } else {
            switch (ch) {
                case KEY_LEFT:
                    if (button_selected > 0) button_selected--;
                    break;
                case KEY_RIGHT:
                    if (button_selected < 1) button_selected++;
                    break;
                case '\t':
                case KEY_UP:
                    editing = true;
                    curs_set(1);
                    break;
                case '\n':
                case KEY_ENTER:
                    curs_set(0);
                    dialog_destroy(dialog);
                    return (button_selected == 0);
                case 27:
                    curs_set(0);
                    dialog_destroy(dialog);
                    return false;
            }
        }
    }
}

bool dialog_input_int(const char *title, const char *prompt, int *value, int min_val, int max_val) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", *value);
    
    char full_prompt[64];
    snprintf(full_prompt, sizeof(full_prompt), "%s (%d-%d)", prompt, min_val, max_val);
    
    if (dialog_input_string(title, full_prompt, buffer, sizeof(buffer))) {
        int new_val = atoi(buffer);
        if (new_val >= min_val && new_val <= max_val) {
            *value = new_val;
            return true;
        }
        dialog_error("Value out of range");
    }
    return false;
}

bool dialog_input_float(const char *title, const char *prompt, float *value, float min_val, float max_val) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.4f", *value);
    
    char full_prompt[64];
    snprintf(full_prompt, sizeof(full_prompt), "%s (%.2f-%.2f)", prompt, min_val, max_val);
    
    if (dialog_input_string(title, full_prompt, buffer, sizeof(buffer))) {
        float new_val = atof(buffer);
        if (new_val >= min_val && new_val <= max_val) {
            *value = new_val;
            return true;
        }
        dialog_error("Value out of range");
    }
    return false;
}

/* ============================================================================
 * Selection Dialogs
 * ========================================================================== */

int dialog_select(const char *title, const char **options, int count, int initial) {
    if (count <= 0 || !options) return -1;
    
    // Calculate dimensions
    int max_len = strlen(title);
    for (int i = 0; i < count; i++) {
        int len = strlen(options[i]);
        if (len > max_len) max_len = len;
    }
    
    int width = max_len + 8;
    int height = MIN(count + 4, 20);
    int visible = height - 4;
    
    WINDOW *dialog = dialog_create(height, width, title);
    if (!dialog) return -1;
    
    int selected = (initial >= 0 && initial < count) ? initial : 0;
    int scroll = 0;
    
    while (1) {
        // Adjust scroll
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + visible) scroll = selected - visible + 1;
        
        // Draw options
        for (int i = 0; i < visible && i + scroll < count; i++) {
            int idx = i + scroll;
            if (idx == selected) {
                wattron(dialog, A_REVERSE);
            }
            mvwprintw(dialog, i + 2, 2, " %-*s ", max_len, options[idx]);
            if (idx == selected) {
                wattroff(dialog, A_REVERSE);
            }
        }
        
        // Scroll indicators
        if (scroll > 0) {
            mvwaddch(dialog, 1, width / 2, ACS_UARROW);
        }
        if (scroll + visible < count) {
            mvwaddch(dialog, height - 2, width / 2, ACS_DARROW);
        }
        
        wrefresh(dialog);
        
        int ch = wgetch(dialog);
        switch (ch) {
            case KEY_UP:
                if (selected > 0) selected--;
                break;
            case KEY_DOWN:
                if (selected < count - 1) selected++;
                break;
            case KEY_PPAGE:
                selected = MAX(0, selected - visible);
                break;
            case KEY_NPAGE:
                selected = MIN(count - 1, selected + visible);
                break;
            case KEY_HOME:
                selected = 0;
                break;
            case KEY_END:
                selected = count - 1;
                break;
            case '\n':
            case KEY_ENTER:
                dialog_destroy(dialog);
                return selected;
            case 27:  // Escape
                dialog_destroy(dialog);
                return -1;
        }
    }
}
