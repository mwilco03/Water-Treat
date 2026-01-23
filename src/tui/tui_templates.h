/**
 * @file tui_templates.h
 * @brief Reusable TUI templates for consistent page and dialog patterns
 *
 * This header provides templated components to reduce boilerplate across
 * TUI pages that follow common patterns:
 *
 * 1. List Page Template - For pages showing lists of items (sensors, actuators, alarms)
 * 2. Form Dialog Template - For add/edit dialogs with field editors
 * 3. Config Page Template - For pages editing configuration fields
 *
 * Usage:
 *   1. Define callbacks for data loading, rendering, and actions
 *   2. Initialize template state
 *   3. Call template draw/input handlers from page handlers
 */

#ifndef TUI_TEMPLATES_H
#define TUI_TEMPLATES_H

#include <ncurses.h>
#include <stdbool.h>
#include "tui_common.h"

/* ============================================================================
 * List Page Template
 *
 * For pages that display a scrollable list of items with CRUD operations.
 * Used by: page_sensors, page_actuators, page_alarms, page_profinet
 * ========================================================================== */

/** Maximum columns in a list display */
#define TUI_LIST_MAX_COLUMNS 8

/** Maximum help text length */
#define TUI_HELP_MAX_LEN 256

/** Column definition for list display */
typedef struct {
    const char *header;     /**< Column header text */
    int width;              /**< Column width (0 = auto) */
    bool right_align;       /**< Right-align values */
} tui_list_column_t;

/** Callback to format a cell value */
typedef void (*tui_list_format_cell_fn)(void *item, int column, char *buffer, size_t size);

/** Callback to get cell color */
typedef int (*tui_list_cell_color_fn)(void *item, int column);

/** Callback to load data items */
typedef int (*tui_list_load_fn)(void **items, int *count);

/** Callback to free loaded items */
typedef void (*tui_list_free_fn)(void *items);

/** Callback for view/add/edit/delete actions */
typedef void (*tui_list_action_fn)(void *item, int index);

/**
 * List page template configuration
 */
typedef struct {
    const char *title;                      /**< Page title */

    /* Column definitions */
    tui_list_column_t columns[TUI_LIST_MAX_COLUMNS];
    int column_count;

    /* Data callbacks */
    tui_list_load_fn load_data;             /**< Load items from database */
    tui_list_free_fn free_data;             /**< Free loaded items */
    tui_list_format_cell_fn format_cell;    /**< Format cell for display */
    tui_list_cell_color_fn get_cell_color;  /**< Get cell color (optional) */

    /* Action callbacks */
    tui_list_action_fn on_view;             /**< View item (Enter) */
    tui_list_action_fn on_add;              /**< Add item (a) */
    tui_list_action_fn on_edit;             /**< Edit item (e) */
    tui_list_action_fn on_delete;           /**< Delete item (d) */
    tui_list_action_fn on_refresh;          /**< Refresh (r) */

    /* Help text */
    const char *help_text;                  /**< Bottom help line */

    /* Item size for array access */
    size_t item_size;                       /**< sizeof(item_struct) */

    /* Visible rows (excluding header/footer) */
    int visible_rows;
} tui_list_page_config_t;

/**
 * List page template state
 */
typedef struct {
    const tui_list_page_config_t *config;
    WINDOW *win;
    void *items;                            /**< Array of items */
    int item_count;
    tui_list_state_t list;                  /**< Scroll/selection state */
    bool needs_reload;
} tui_list_page_state_t;

/**
 * Initialize list page template
 */
void tui_list_page_init(tui_list_page_state_t *state,
                        const tui_list_page_config_t *config,
                        WINDOW *win);

/**
 * Load/reload data for list page
 */
void tui_list_page_load(tui_list_page_state_t *state);

/**
 * Draw list page using template
 */
void tui_list_page_draw(tui_list_page_state_t *state);

/**
 * Handle input for list page
 * @return true if key was handled
 */
bool tui_list_page_input(tui_list_page_state_t *state, int ch);

/**
 * Cleanup list page template
 */
void tui_list_page_cleanup(tui_list_page_state_t *state);

/**
 * Get currently selected item
 * @return Pointer to selected item or NULL
 */
void *tui_list_page_selected(tui_list_page_state_t *state);

/**
 * Get selected item index
 */
int tui_list_page_selected_index(tui_list_page_state_t *state);

/* ============================================================================
 * Form Dialog Template
 *
 * For dialogs with labeled fields that can be edited.
 * Used by: dialog_sensor, dialog_actuator, dialog_alarm
 * ========================================================================== */

/** Maximum fields in a form */
#define TUI_FORM_MAX_FIELDS 20

/** Field types */
typedef enum {
    TUI_FIELD_TEXT,         /**< Text input */
    TUI_FIELD_NUMBER,       /**< Numeric input */
    TUI_FIELD_BOOL,         /**< Yes/No toggle */
    TUI_FIELD_SELECT,       /**< Selection from list */
    TUI_FIELD_READONLY,     /**< Display only */
    TUI_FIELD_SEPARATOR     /**< Visual separator (no value) */
} tui_field_type_t;

/** Field definition */
typedef struct {
    const char *label;              /**< Field label */
    tui_field_type_t type;          /**< Field type */
    int max_length;                 /**< Max input length (for text) */
    const char **options;           /**< Options for SELECT type */
    int option_count;               /**< Number of options */
    bool required;                  /**< Field is required */
    const char *help;               /**< Field help text (optional) */
} tui_form_field_def_t;

/** Field value union */
typedef union {
    char text[256];
    int number;
    bool boolean;
    int selected;           /**< Index for SELECT type */
} tui_field_value_t;

/** Callback for field validation */
typedef bool (*tui_form_validate_fn)(const tui_field_value_t *values, int field_count,
                                     char *error_msg, size_t error_size);

/** Callback when form is submitted */
typedef bool (*tui_form_submit_fn)(const tui_field_value_t *values, int field_count,
                                   void *user_data);

/**
 * Form dialog configuration
 */
typedef struct {
    const char *title;                      /**< Dialog title */
    tui_form_field_def_t fields[TUI_FORM_MAX_FIELDS];
    int field_count;
    tui_form_validate_fn validate;          /**< Optional validation */
    tui_form_submit_fn on_submit;           /**< Submit callback */
    void *user_data;                        /**< User data for callbacks */
    int width;                              /**< Dialog width (0 = auto) */
    int height;                             /**< Dialog height (0 = auto) */
} tui_form_config_t;

/**
 * Form dialog state
 */
typedef struct {
    const tui_form_config_t *config;
    WINDOW *win;
    tui_field_value_t values[TUI_FORM_MAX_FIELDS];
    int current_field;
    bool editing;
    char status_msg[128];
} tui_form_state_t;

/**
 * Show form dialog
 * @param config Form configuration
 * @param initial_values Initial field values (NULL for defaults)
 * @return true if form was submitted, false if cancelled
 */
bool tui_form_dialog_show(const tui_form_config_t *config,
                          tui_field_value_t *initial_values);

/**
 * Initialize form state (for custom rendering)
 */
void tui_form_init(tui_form_state_t *state, const tui_form_config_t *config);

/**
 * Draw form (for custom rendering)
 */
void tui_form_draw(tui_form_state_t *state);

/**
 * Handle form input (for custom rendering)
 * @return 1 = submitted, -1 = cancelled, 0 = continue
 */
int tui_form_input(tui_form_state_t *state, int ch);

/* ============================================================================
 * Config Page Template
 *
 * For pages that edit configuration settings with save/cancel.
 * Used by: page_system, page_network, page_logging
 * ========================================================================== */

/** Config field definition */
typedef struct {
    const char *label;              /**< Field label */
    const char *section;            /**< Config file section */
    const char *key;                /**< Config file key */
    tui_field_type_t type;          /**< Field type */
    const char *default_value;      /**< Default value */
    const char **options;           /**< Options for SELECT type */
    int option_count;
    bool readonly;                  /**< Display only */
} tui_config_field_def_t;

/**
 * Config page configuration
 */
typedef struct {
    const char *title;                      /**< Page title */
    tui_config_field_def_t *fields;         /**< Field definitions */
    int field_count;
    const char *help_text;                  /**< Bottom help line */
} tui_config_page_config_t;

/**
 * Config page state
 */
typedef struct {
    const tui_config_page_config_t *config;
    WINDOW *win;
    char values[TUI_FORM_MAX_FIELDS][256];  /**< Current values */
    char original[TUI_FORM_MAX_FIELDS][256]; /**< Original values for undo */
    int current_field;
    bool modified;
} tui_config_page_state_t;

/**
 * Initialize config page
 */
void tui_config_page_init(tui_config_page_state_t *state,
                          const tui_config_page_config_t *config,
                          WINDOW *win);

/**
 * Load config values from config manager
 */
void tui_config_page_load(tui_config_page_state_t *state);

/**
 * Draw config page
 */
void tui_config_page_draw(tui_config_page_state_t *state);

/**
 * Handle config page input
 * @return true if key was handled
 */
bool tui_config_page_input(tui_config_page_state_t *state, int ch);

/**
 * Save config values to config manager
 */
bool tui_config_page_save(tui_config_page_state_t *state);

/**
 * Check if config page has unsaved changes
 */
bool tui_config_page_modified(tui_config_page_state_t *state);

/* ============================================================================
 * Helper Macros for Template Definition
 * ========================================================================== */

/** Define a list column */
#define TUI_COL(hdr, w) { .header = (hdr), .width = (w), .right_align = false }
#define TUI_COL_RIGHT(hdr, w) { .header = (hdr), .width = (w), .right_align = true }

/** Define a form field */
#define TUI_FIELD_TEXT(lbl, maxlen) \
    { .label = (lbl), .type = TUI_FIELD_TEXT, .max_length = (maxlen) }

#define TUI_FIELD_NUM(lbl) \
    { .label = (lbl), .type = TUI_FIELD_NUMBER }

#define TUI_FIELD_BOOL_DEF(lbl) \
    { .label = (lbl), .type = TUI_FIELD_BOOL }

#define TUI_FIELD_SELECT_DEF(lbl, opts, cnt) \
    { .label = (lbl), .type = TUI_FIELD_SELECT, .options = (opts), .option_count = (cnt) }

#define TUI_FIELD_RO(lbl) \
    { .label = (lbl), .type = TUI_FIELD_READONLY }

#define TUI_FIELD_SEP() \
    { .label = NULL, .type = TUI_FIELD_SEPARATOR }

#endif /* TUI_TEMPLATES_H */
