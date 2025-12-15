#ifndef WIDGET_DIALOG_H
#define WIDGET_DIALOG_H

#include "common.h"
#include <ncurses.h>

#define MAX_FORM_FIELDS 32
#define MAX_TABLE_COLUMNS 10
#define MAX_TABLE_ROWS 256

typedef enum {
    FIELD_TYPE_STRING,
    FIELD_TYPE_INT,
    FIELD_TYPE_FLOAT,
    FIELD_TYPE_BOOL,
    FIELD_TYPE_SELECT,
    FIELD_TYPE_PASSWORD
} field_type_t;

typedef struct {
    char label[32];
    field_type_t type;
    union {
        char str_val[256];
        int int_val;
        float float_val;
        bool bool_val;
    } value;
    int max_len;
    bool editable;
    bool visible;
    const char **options;
    int option_count;
    union { int i; float f; } min_val;
    union { int i; float f; } max_val;
} form_field_t;

typedef struct {
    char title[64];
    int width, height;
    form_field_t *fields[MAX_FORM_FIELDS];
    int field_count;
    int selected;
    int scroll_offset;
} form_t;

typedef struct {
    char *cells[MAX_TABLE_COLUMNS];
    int color;
} table_row_t;

typedef struct {
    int columns;
    char headers[MAX_TABLE_COLUMNS][32];
    int col_widths[MAX_TABLE_COLUMNS];
    table_row_t rows[MAX_TABLE_ROWS];
    int row_count;
    int selected;
    int scroll_offset;
} table_t;

/* Form field functions */
form_field_t* form_field_create(const char *label, field_type_t type, int max_len);
void form_field_destroy(form_field_t *field);
void form_field_set_string(form_field_t *field, const char *value);
void form_field_set_int(form_field_t *field, int value);
void form_field_set_float(form_field_t *field, float value);
void form_field_set_bool(form_field_t *field, bool value);
void form_field_set_options(form_field_t *field, const char **options, int count);
void form_field_set_range_int(form_field_t *field, int min_val, int max_val);
void form_field_set_range_float(form_field_t *field, float min_val, float max_val);
void form_field_draw(WINDOW *win, form_field_t *field, int y, int x, int label_width,
                     bool selected, bool editing);
bool form_field_edit(WINDOW *win, form_field_t *field, int y, int x);

/* Form functions */
form_t* form_create(const char *title, int width, int height);
void form_destroy(form_t *form);
bool form_add_field(form_t *form, form_field_t *field);
form_field_t* form_get_field(form_t *form, int index);
form_field_t* form_get_field_by_label(form_t *form, const char *label);
void form_draw(WINDOW *win, form_t *form, int y, int x, bool editing);
bool form_handle_input(form_t *form, WINDOW *win, int ch);

/* Table functions */
table_t* table_create(int columns);
void table_destroy(table_t *table);
void table_set_header(table_t *table, int col, const char *header, int width);
int table_add_row(table_t *table);
void table_set_cell(table_t *table, int row, int col, const char *value);
void table_set_row_color(table_t *table, int row, int color);
void table_clear(table_t *table);
void table_draw(WINDOW *win, table_t *table, int y, int x, int visible_rows);
bool table_handle_input(table_t *table, int ch, int visible_rows);

#endif
