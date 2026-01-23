/**
 * @file page_sensors.c
 * @brief Sensor management page - refactored to use tui_templates
 */

#include "page_sensors.h"
#include "../tui_common.h"
#include "../tui_templates.h"
#include "../dialogs/dialog_sensor.h"
#include "../dialogs/dialog_io_wizard.h"
#include "db/database.h"
#include "db/db_modules.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>

#define VISIBLE_ROWS 15

/* Sensor item structure - matches db_module_with_status_t layout for efficiency */
typedef struct {
    int id;
    int slot;
    char name[64];
    char type[32];
    char status[16];
    float value;
} sensor_item_t;

/* ============================================================================
 * Template Callbacks
 * ========================================================================== */

static int sensors_load_data(void **items, int *count) {
    database_t *db = tui_get_database();
    if (!db) {
        *items = NULL;
        *count = 0;
        return 0;
    }

    /* Use optimized JOIN query - single query instead of N+1 */
    db_module_with_status_t *modules = NULL;
    int module_count = 0;

    if (db_module_list_with_status(db, &modules, &module_count) != RESULT_OK || !modules) {
        *items = NULL;
        *count = 0;
        return 0;
    }

    /* Allocate sensor items array */
    sensor_item_t *sensors = calloc(module_count, sizeof(sensor_item_t));
    if (!sensors) {
        free(modules);
        *items = NULL;
        *count = 0;
        return 0;
    }

    /* Copy module data to sensor items */
    for (int i = 0; i < module_count; i++) {
        sensors[i].id = modules[i].module.id;
        sensors[i].slot = modules[i].module.slot;
        SAFE_STRNCPY(sensors[i].name, modules[i].module.name, sizeof(sensors[i].name));
        SAFE_STRNCPY(sensors[i].type, modules[i].module.module_type, sizeof(sensors[i].type));
        sensors[i].value = modules[i].value;
        SAFE_STRNCPY(sensors[i].status, modules[i].sensor_status, sizeof(sensors[i].status));
    }

    free(modules);
    *items = sensors;
    *count = module_count;
    return module_count;
}

static void sensors_free_data(void *items) {
    free(items);
}

static void sensors_format_cell(void *item, int column, char *buffer, size_t size) {
    sensor_item_t *s = (sensor_item_t *)item;

    switch (column) {
        case 0: snprintf(buffer, size, "%d", s->slot); break;
        case 1: SAFE_STRNCPY(buffer, s->name, size); break;
        case 2: SAFE_STRNCPY(buffer, s->type, size); break;
        case 3: snprintf(buffer, size, "%.2f", s->value); break;
        case 4: SAFE_STRNCPY(buffer, s->status, size); break;
        default: buffer[0] = '\0'; break;
    }
}

static int sensors_get_cell_color(void *item, int column) {
    sensor_item_t *s = (sensor_item_t *)item;
    /* Apply status color to value and status columns */
    if (column == 3 || column == 4) {
        return tui_status_color(s->status);
    }
    return TUI_COLOR_NORMAL;
}

/* Forward declarations for action callbacks */
static void sensors_on_view(void *item, int index);
static void sensors_on_add(void *item, int index);
static void sensors_on_edit(void *item, int index);
static void sensors_on_delete(void *item, int index);

/* ============================================================================
 * Template Configuration
 * ========================================================================== */

static const tui_list_page_config_t sensors_config = {
    .title = "Sensors",
    .columns = {
        TUI_COL("Slot", 4),
        TUI_COL("Name", 20),
        TUI_COL("Type", 12),
        TUI_COL_RIGHT("Value", 10),
        TUI_COL("Status", 12),
    },
    .column_count = 5,
    .load_data = sensors_load_data,
    .free_data = sensors_free_data,
    .format_cell = sensors_format_cell,
    .get_cell_color = sensors_get_cell_color,
    .on_view = sensors_on_view,
    .on_add = sensors_on_add,
    .on_edit = sensors_on_edit,
    .on_delete = sensors_on_delete,
    .on_refresh = NULL,  /* Default reload behavior is sufficient */
    .help_text = "a:Add  e:Edit  d:Delete  Enter:View  r:Refresh  Arrows:Navigate",
    .item_size = sizeof(sensor_item_t),
    .visible_rows = VISIBLE_ROWS,
};

/* ============================================================================
 * Page State
 * ========================================================================== */

static tui_list_page_state_t g_state = {0};

/* ============================================================================
 * Action Callbacks
 * ========================================================================== */

static void sensors_on_view(void *item, int index) {
    UNUSED(index);
    sensor_item_t *s = (sensor_item_t *)item;

    database_t *db = tui_get_database();
    if (!db) return;

    WINDOW *dialog = newwin(20, 60, 4, 10);
    box(dialog, 0, 0);

    wattron(dialog, A_BOLD);
    mvwprintw(dialog, 0, 20, " Sensor Details ");
    wattroff(dialog, A_BOLD);

    int row = 2;
    mvwprintw(dialog, row++, 2, "Slot:     %d", s->slot);
    mvwprintw(dialog, row++, 2, "Name:     %s", s->name);
    mvwprintw(dialog, row++, 2, "Type:     %s", s->type);
    mvwprintw(dialog, row++, 2, "Value:    %.4f", s->value);
    mvwprintw(dialog, row++, 2, "Status:   %s", s->status);

    row++;

    /* Get sensor-specific details */
    db_physical_sensor_t phys;
    if (db_physical_sensor_get(db, s->id, &phys) == RESULT_OK) {
        mvwprintw(dialog, row++, 2, "Interface: %s", phys.interface);
        mvwprintw(dialog, row++, 2, "Address:   %s", phys.address);
        mvwprintw(dialog, row++, 2, "Bus:       %d", phys.bus);
        mvwprintw(dialog, row++, 2, "Channel:   %d", phys.channel);
        mvwprintw(dialog, row++, 2, "Poll Rate: %d ms", phys.poll_rate_ms);
    }

    db_adc_sensor_t adc;
    if (db_adc_sensor_get(db, s->id, &adc) == RESULT_OK) {
        mvwprintw(dialog, row++, 2, "ADC Type:  %s", adc.adc_type);
        mvwprintw(dialog, row++, 2, "Channel:   %d", adc.channel);
        mvwprintw(dialog, row++, 2, "Gain:      %d", adc.gain);
        mvwprintw(dialog, row++, 2, "Range:     %.2f - %.2f %s", adc.eng_min, adc.eng_max, adc.unit);
    }

    row++;
    wattron(dialog, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwprintw(dialog, row, 2, "Press any key to close");
    wattroff(dialog, COLOR_PAIR(TUI_COLOR_NORMAL));

    wrefresh(dialog);
    wgetch(dialog);
    delwin(dialog);
}

static void sensors_on_add(void *item, int index) {
    UNUSED(item);
    UNUSED(index);

    /*
     * Use the progressive disclosure I/O wizard.
     *
     * Design Philosophy Applied:
     * - Dynamic Discovery: Wizard scans I2C/1-Wire before asking questions
     * - Reasonable Assumptions: System infers technical details from user choices
     * - Graceful Degradation: Conflicts shown, not blocked
     * - Single Source of Truth: User points at device, system derives config
     * - Informational Output: Shows what was discovered
     */
    io_wizard_result_t result;
    if (dialog_io_wizard_add_sensor(&result)) {
        /* Sensor was created - template will reload, notify PROFINET */
        tui_notify_sensor_changed(-1);  /* -1 = all sensors changed */
        tui_set_status("Added sensor '%s' at slot %d", result.name, result.assigned_slot);

        /* Select the newly added sensor after reload */
        tui_list_page_load(&g_state);
        sensor_item_t *sensors = (sensor_item_t *)g_state.items;
        for (int i = 0; i < g_state.item_count; i++) {
            if (sensors[i].id == result.created_id) {
                g_state.list.selected = i;
                break;
            }
        }
    }
}

static void sensors_on_edit(void *item, int index) {
    UNUSED(index);
    sensor_item_t *s = (sensor_item_t *)item;
    int slot = s->slot;
    char name[64];
    SAFE_STRNCPY(name, s->name, sizeof(name));

    if (dialog_sensor_edit(s->id)) {
        /* Sensor was updated - template will reload, notify PROFINET */
        tui_notify_sensor_changed(slot);
        tui_set_status("Updated sensor: %s", name);
    }
}

static void sensors_on_delete(void *item, int index) {
    UNUSED(index);
    sensor_item_t *s = (sensor_item_t *)item;
    int slot = s->slot;
    char name[64];
    SAFE_STRNCPY(name, s->name, sizeof(name));

    if (dialog_sensor_delete(s->id)) {
        /* Sensor was deleted - template will reload, notify PROFINET */
        tui_notify_sensor_changed(slot);
        tui_set_status("Deleted sensor: %s", name);
    }
}

/* ============================================================================
 * Page Interface Functions
 * ========================================================================== */

void page_sensors_init(WINDOW *win) {
    tui_list_page_init(&g_state, &sensors_config, win);
    tui_list_page_load(&g_state);
}

void page_sensors_draw(WINDOW *win) {
    UNUSED(win);

    /* Reload if needed (after add/edit/delete) */
    if (g_state.needs_reload) {
        tui_list_page_load(&g_state);
    }

    tui_list_page_draw(&g_state);
}

void page_sensors_input(WINDOW *win, int ch) {
    UNUSED(win);
    tui_list_page_input(&g_state, ch);
}

void page_sensors_cleanup(void) {
    tui_list_page_cleanup(&g_state);
}
