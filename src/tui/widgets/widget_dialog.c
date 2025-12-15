/**
 * @file widget_dialog.c
 * @brief Reusable dialog widget components
 */

#include "widget_dialog.h"
#include "../tui_common.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * Form Field Widget
 * ========================================================================== */

form_field_t* form_field_create(const char *label, field_type_t type, int max_len) {
    form_field_t *field = calloc(1, sizeof(form_field_t));
    if (!field) return NULL;
    
    SAFE_STRNCPY(field->label, label, sizeof(field->label));
    field->type = type;
    field->max_len = max_len > 0 ? MIN(max_len, 255) : 64;
    field->editable = true;
    field->visible = true;
    
    return field;
}

void form_field_destroy(form_field_t *field) {
    if (field) {
        if (field->options) free(field->options);
        free(field);
    }
}

void form_field_set_string(form_field_t *field, const char *value) {
    if (field && value) {
        SAFE_STRNCPY(field->value.str_val, value, sizeof(field->value.str_val));
    }
}

void form_field_set_int(form_field_t *field, int value) {
    if (field) field->value.int_val = value;
}

void form_field_set_float(form_field_t *field, float value) {
    if (field) field->value.float_val = value;
}

void form_field_set_bool(form_field_t *field, bool value) {
    if (field) field->value.bool_val = value;
}

void form_field_set_options(form_field_t *field, const char **options, int count) {
    if (!field || !options || count <= 0) return;
    
    if (field->options) free(field->options);
    
    field->options = calloc(count, sizeof(char *));
    if (!field->options) return;
    
    field->option_count = count;
    for (int i = 0; i < count; i++) {
        field->options[i] = options[i];
    }
}

void form_field_set_range_int(form_field_t *field, int min_val, int max_val) {
    if (field) {
        field->min_val.i = min_val;
        field->max_val.i = max_val;
    }
}

void form_field_set_range_float(form_field_t *field, float min_val, float max_val) {
    if (field) {
        field->min_val.f = min_val;
        field->max_val.f = max_val;
    }
}

void form_field_draw(WINDOW *win, form_field_t *field, int y, int x, int label_width, 
                     bool selected, bool editing) {
    if (!field || !field->visible) return;
    
    // Label
    if (selected && !editing) {
        wattron(win, A_REVERSE);
    }
    
    mvwprintw(win, y, x, "%-*s:", label_width, field->label);
    
    // Value
    int value_x = x + label_width + 2;
    char display[64] = "";
    
    switch (field->type) {
        case FIELD_TYPE_STRING:
            strncpy(display, field->value.str_val, sizeof(display) - 1);
            break;
        case FIELD_TYPE_INT:
            snprintf(display, sizeof(display), "%d", field->value.int_val);
            break;
        case FIELD_TYPE_FLOAT:
            snprintf(display, sizeof(display), "%.4f", field->value.float_val);
            break;
        case FIELD_TYPE_BOOL:
            strcpy(display, field->value.bool_val ? "Yes" : "No");
            break;
        case FIELD_TYPE_SELECT:
            if (field->value.int_val >= 0 && field->value.int_val < field->option_count) {
                strncpy(display, field->options[field->value.int_val], sizeof(display) - 1);
            }
            break;
        case FIELD_TYPE_PASSWORD:
            memset(display, '*', MIN(strlen(field->value.str_val), sizeof(display) - 1));
            break;
    }
    
    if (editing) {
        wattron(win, COLOR_PAIR(TUI_COLOR_INPUT));
    } else if (field->editable) {
        wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
    }
    
    mvwprintw(win, y, value_x, "%-30s", display);
    
    if (editing) {
        wattroff(win, COLOR_PAIR(TUI_COLOR_INPUT));
    } else if (field->editable) {
        wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
    }
    
    if (selected && !editing) {
        wattroff(win, A_REVERSE);
    }
}

bool form_field_edit(WINDOW *win, form_field_t *field, int y, int x) {
    if (!field || !field->editable) return false;
    
    int value_x = x + strlen(field->label) + 4;
    
    switch (field->type) {
        case FIELD_TYPE_STRING:
        case FIELD_TYPE_PASSWORD:
            return tui_get_string(win, y, value_x, field->value.str_val, field->max_len, field->value.str_val) == 1;
            
        case FIELD_TYPE_INT:
            return tui_get_int(win, y, value_x, &field->value.int_val, field->min_val.i, field->max_val.i) == 1;
            
        case FIELD_TYPE_FLOAT:
            return tui_get_float(win, y, value_x, &field->value.float_val, field->min_val.f, field->max_val.f) == 1;
            
        case FIELD_TYPE_BOOL:
            field->value.bool_val = !field->value.bool_val;
            return true;
            
        case FIELD_TYPE_SELECT:
            if (field->options && field->option_count > 0) {
                int sel = tui_menu(win, field->label, (const char **)field->options, field->option_count);
                if (sel >= 0) {
                    field->value.int_val = sel;
                    return true;
                }
            }
            break;
    }
    
    return false;
}

/* ============================================================================
 * Form Widget
 * ========================================================================== */

form_t* form_create(const char *title, int width, int height) {
    form_t *form = calloc(1, sizeof(form_t));
    if (!form) return NULL;
    
    SAFE_STRNCPY(form->title, title, sizeof(form->title));
    form->width = width > 20 ? width : 60;
    form->height = height > 5 ? height : 20;
    form->selected = 0;
    form->scroll_offset = 0;
    
    return form;
}

void form_destroy(form_t *form) {
    if (form) {
        for (int i = 0; i < form->field_count; i++) {
            form_field_destroy(form->fields[i]);
        }
        free(form);
    }
}

bool form_add_field(form_t *form, form_field_t *field) {
    if (!form || !field || form->field_count >= MAX_FORM_FIELDS) return false;
    
    form->fields[form->field_count++] = field;
    return true;
}

form_field_t* form_get_field(form_t *form, int index) {
    if (!form || index < 0 || index >= form->field_count) return NULL;
    return form->fields[index];
}

form_field_t* form_get_field_by_label(form_t *form, const char *label) {
    if (!form || !label) return NULL;
    
    for (int i = 0; i < form->field_count; i++) {
        if (strcmp(form->fields[i]->label, label) == 0) {
            return form->fields[i];
        }
    }
    return NULL;
}

void form_draw(WINDOW *win, form_t *form, int y, int x, bool editing) {
    if (!form) return;
    
    int visible = form->height - 4;
    int label_width = 0;
    
    // Find max label width
    for (int i = 0; i < form->field_count; i++) {
        int len = strlen(form->fields[i]->label);
        if (len > label_width) label_width = len;
    }
    
    for (int i = 0; i < visible && i + form->scroll_offset < form->field_count; i++) {
        int idx = i + form->scroll_offset;
        form_field_draw(win, form->fields[idx], y + i, x, label_width,
                       idx == form->selected, editing && idx == form->selected);
    }
}

bool form_handle_input(form_t *form, WINDOW *win, int ch) {
    if (!form) return false;
    
    int visible = form->height - 4;
    
    switch (ch) {
        case KEY_UP:
            if (form->selected > 0) {
                form->selected--;
                if (form->selected < form->scroll_offset) {
                    form->scroll_offset = form->selected;
                }
            }
            return true;
            
        case KEY_DOWN:
            if (form->selected < form->field_count - 1) {
                form->selected++;
                if (form->selected >= form->scroll_offset + visible) {
                    form->scroll_offset = form->selected - visible + 1;
                }
            }
            return true;
            
        case KEY_PPAGE:
            form->selected = MAX(0, form->selected - visible);
            form->scroll_offset = MAX(0, form->scroll_offset - visible);
            return true;
            
        case KEY_NPAGE:
            form->selected = MIN(form->field_count - 1, form->selected + visible);
            form->scroll_offset = MIN(MAX(0, form->field_count - visible),
                                     form->scroll_offset + visible);
            return true;
            
        case '\n':
        case KEY_ENTER:
            if (form->fields[form->selected]->editable) {
                return form_field_edit(win, form->fields[form->selected], 
                                      2 + form->selected - form->scroll_offset, 2);
            }
            return false;
    }
    
    return false;
}

/* ============================================================================
 * Table Widget
 * ========================================================================== */

table_t* table_create(int columns) {
    if (columns <= 0 || columns > MAX_TABLE_COLUMNS) return NULL;
    
    table_t *table = calloc(1, sizeof(table_t));
    if (!table) return NULL;
    
    table->columns = columns;
    return table;
}

void table_destroy(table_t *table) {
    if (table) {
        for (int i = 0; i < table->row_count; i++) {
            for (int j = 0; j < table->columns; j++) {
                if (table->rows[i].cells[j]) {
                    free(table->rows[i].cells[j]);
                }
            }
        }
        free(table);
    }
}

void table_set_header(table_t *table, int col, const char *header, int width) {
    if (!table || col < 0 || col >= table->columns) return;
    
    SAFE_STRNCPY(table->headers[col], header, sizeof(table->headers[col]));
    table->col_widths[col] = width > 0 ? width : strlen(header) + 2;
}

int table_add_row(table_t *table) {
    if (!table || table->row_count >= MAX_TABLE_ROWS) return -1;
    
    int idx = table->row_count++;
    memset(&table->rows[idx], 0, sizeof(table_row_t));
    return idx;
}

void table_set_cell(table_t *table, int row, int col, const char *value) {
    if (!table || row < 0 || row >= table->row_count || col < 0 || col >= table->columns) return;
    
    if (table->rows[row].cells[col]) {
        free(table->rows[row].cells[col]);
    }
    table->rows[row].cells[col] = strdup(value ? value : "");
}

void table_set_row_color(table_t *table, int row, int color) {
    if (!table || row < 0 || row >= table->row_count) return;
    table->rows[row].color = color;
}

void table_clear(table_t *table) {
    if (!table) return;
    
    for (int i = 0; i < table->row_count; i++) {
        for (int j = 0; j < table->columns; j++) {
            if (table->rows[i].cells[j]) {
                free(table->rows[i].cells[j]);
                table->rows[i].cells[j] = NULL;
            }
        }
    }
    table->row_count = 0;
    table->selected = 0;
    table->scroll_offset = 0;
}

void table_draw(WINDOW *win, table_t *table, int y, int x, int visible_rows) {
    if (!table) return;
    
    int row = y;
    
    // Draw header
    wattron(win, A_BOLD);
    int col_x = x;
    for (int c = 0; c < table->columns; c++) {
        mvwprintw(win, row, col_x, "%-*s", table->col_widths[c], table->headers[c]);
        col_x += table->col_widths[c] + 1;
    }
    wattroff(win, A_BOLD);
    row++;
    
    // Divider
    mvwhline(win, row++, x, ACS_HLINE, col_x - x - 1);
    
    // Draw rows
    for (int i = 0; i < visible_rows && i + table->scroll_offset < table->row_count; i++) {
        int idx = i + table->scroll_offset;
        
        if (idx == table->selected) {
            wattron(win, A_REVERSE);
        }
        
        if (table->rows[idx].color > 0) {
            wattron(win, COLOR_PAIR(table->rows[idx].color));
        }
        
        col_x = x;
        for (int c = 0; c < table->columns; c++) {
            const char *val = table->rows[idx].cells[c] ? table->rows[idx].cells[c] : "";
            mvwprintw(win, row, col_x, "%-*s", table->col_widths[c], val);
            col_x += table->col_widths[c] + 1;
        }
        
        if (table->rows[idx].color > 0) {
            wattroff(win, COLOR_PAIR(table->rows[idx].color));
        }
        
        if (idx == table->selected) {
            wattroff(win, A_REVERSE);
        }
        
        row++;
    }
}

bool table_handle_input(table_t *table, int ch, int visible_rows) {
    if (!table) return false;
    
    switch (ch) {
        case KEY_UP:
            if (table->selected > 0) {
                table->selected--;
                if (table->selected < table->scroll_offset) {
                    table->scroll_offset = table->selected;
                }
                return true;
            }
            break;
            
        case KEY_DOWN:
            if (table->selected < table->row_count - 1) {
                table->selected++;
                if (table->selected >= table->scroll_offset + visible_rows) {
                    table->scroll_offset = table->selected - visible_rows + 1;
                }
                return true;
            }
            break;
            
        case KEY_PPAGE:
            table->selected = MAX(0, table->selected - visible_rows);
            table->scroll_offset = MAX(0, table->scroll_offset - visible_rows);
            return true;
            
        case KEY_NPAGE:
            table->selected = MIN(table->row_count - 1, table->selected + visible_rows);
            table->scroll_offset = MIN(MAX(0, table->row_count - visible_rows),
                                      table->scroll_offset + visible_rows);
            return true;
            
        case KEY_HOME:
            table->selected = 0;
            table->scroll_offset = 0;
            return true;
            
        case KEY_END:
            table->selected = table->row_count - 1;
            table->scroll_offset = MAX(0, table->row_count - visible_rows);
            return true;
    }
    
    return false;
}
