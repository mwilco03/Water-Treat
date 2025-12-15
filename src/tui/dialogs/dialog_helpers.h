#ifndef DIALOG_HELPERS_H
#define DIALOG_HELPERS_H

#include "common.h"
#include <ncurses.h>

typedef enum {
    DIALOG_YES = 0,
    DIALOG_NO,
    DIALOG_CANCEL
} dialog_result_t;

/* Dialog window management */
WINDOW* dialog_create(int height, int width, const char *title);
WINDOW* dialog_create_at(int y, int x, int height, int width, const char *title);
void dialog_destroy(WINDOW *dialog);
void dialog_draw_button(WINDOW *dialog, int y, int x, const char *label, bool selected);
void dialog_draw_buttons(WINDOW *dialog, int y, const char **labels, int count, int selected);

/* Message dialogs */
void dialog_message(const char *title, const char *message);
void dialog_error(const char *message);
void dialog_warning(const char *message);
void dialog_info(const char *message);
bool dialog_confirm(const char *title, const char *message);
dialog_result_t dialog_yes_no_cancel(const char *title, const char *message);

/* Input dialogs */
bool dialog_input_string(const char *title, const char *prompt, char *buffer, int max_len);
bool dialog_input_int(const char *title, const char *prompt, int *value, int min_val, int max_val);
bool dialog_input_float(const char *title, const char *prompt, float *value, float min_val, float max_val);

/* Selection dialogs */
int dialog_select(const char *title, const char **options, int count, int initial);

#endif
