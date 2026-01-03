/**
 * @file dialog_sensor.c
 * @brief Sensor add/edit dialog
 */

#include "dialog_sensor.h"
#include "dialog_helpers.h"
#include "../tui_common.h"
#include "db/database.h"
#include "db/db_modules.h"
#include "db/db_actuators.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>

#define DIALOG_WIDTH 65
#define DIALOG_HEIGHT 22

typedef struct {
    char name[64];
    int slot;
    int subslot;
    char module_type[32];
    uint32_t module_ident;
    uint32_t submodule_ident;
    char sensor_type[32];
    char interface[16];
    char address[32];
    int bus;
    int channel;
    int gain;
    float reference_voltage;
    char unit[16];
    float min_value;
    float max_value;
    int poll_rate_ms;
} sensor_form_t;

static const char *sensor_types[] = {"physical", "adc", "web_poll", "calculated", "static"};
static const char *interface_types[] = {"i2c", "spi", "gpio", "1wire", "uart"};
static const char *hardware_types[] = {
    "ADS1115", "MCP3008", "DS18B20", "DHT22", "BME280", "HX711",
    "TCS34725", "JSN-SR04T", "Flow Sensor", "pH Sensor", "TDS Sensor",
    "Turbidity", "Float Switch", "Generic"
};

static void init_form(sensor_form_t *form) {
    memset(form, 0, sizeof(*form));
    form->slot = 1;
    form->subslot = 1;
    form->module_ident = 0x00000001;
    form->submodule_ident = 0x00000001;
    strcpy(form->module_type, "physical");
    strcpy(form->interface, "i2c");
    form->reference_voltage = 3.3f;
    form->poll_rate_ms = 1000;
    form->max_value = 100.0f;
}

static void draw_form(WINDOW *dialog, sensor_form_t *form, int selected, bool editing) {
    int row = 2;
    const char *fields[] = {
        "Name", "Slot", "Subslot", "Type", "Hardware", "Interface",
        "Address", "Bus", "Channel", "Gain", "Ref Voltage",
        "Unit", "Min Value", "Max Value", "Poll Rate (ms)"
    };
    
    for (int i = 0; i < 15; i++) {
        if (i == selected) wattron(dialog, A_REVERSE);
        mvwprintw(dialog, row, 2, "%-18s:", fields[i]);
        
        char value[64] = "";
        switch (i) {
            case 0: strncpy(value, form->name, 63); break;
            case 1: snprintf(value, 63, "%d", form->slot); break;
            case 2: snprintf(value, 63, "%d", form->subslot); break;
            case 3: strncpy(value, form->module_type, 63); break;
            case 4: strncpy(value, form->sensor_type, 63); break;
            case 5: strncpy(value, form->interface, 63); break;
            case 6: strncpy(value, form->address, 63); break;
            case 7: snprintf(value, 63, "%d", form->bus); break;
            case 8: snprintf(value, 63, "%d", form->channel); break;
            case 9: snprintf(value, 63, "%d", form->gain); break;
            case 10: snprintf(value, 63, "%.2f", form->reference_voltage); break;
            case 11: strncpy(value, form->unit, 63); break;
            case 12: snprintf(value, 63, "%.2f", form->min_value); break;
            case 13: snprintf(value, 63, "%.2f", form->max_value); break;
            case 14: snprintf(value, 63, "%d", form->poll_rate_ms); break;
        }
        
        wattron(dialog, COLOR_PAIR(editing && i == selected ? TUI_COLOR_INPUT : TUI_COLOR_STATUS));
        mvwprintw(dialog, row, 22, "%-30s", value);
        wattroff(dialog, COLOR_PAIR(editing && i == selected ? TUI_COLOR_INPUT : TUI_COLOR_STATUS));
        
        if (i == selected) wattroff(dialog, A_REVERSE);
        row++;
    }
    
    row++;
    wattron(dialog, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwprintw(dialog, row++, 2, "Up/Down: Navigate  Enter: Edit/Select");
    mvwprintw(dialog, row, 2, "Tab: Buttons  Esc: Cancel");
    wattroff(dialog, COLOR_PAIR(TUI_COLOR_NORMAL));
}

static bool edit_field(WINDOW *dialog, sensor_form_t *form, int field) {
    char buffer[64];
    int row = 2 + field, col = 22;
    
    switch (field) {
        case 0:
            strncpy(buffer, form->name, 63);
            if (tui_get_string(dialog, row, col, buffer, 30, buffer)) {
                strncpy(form->name, buffer, 63);
                return true;
            }
            break;
        case 1: return tui_get_int(dialog, row, col, &form->slot, 1, 63);
        case 2: return tui_get_int(dialog, row, col, &form->subslot, 1, 63);
        case 3:
            {
                int sel = dialog_select("Select Type", sensor_types, 5, 0);
                if (sel >= 0) { strncpy(form->module_type, sensor_types[sel], 31); return true; }
            }
            break;
        case 4:
            {
                int sel = dialog_select("Select Hardware", hardware_types, 14, 0);
                if (sel >= 0) { strncpy(form->sensor_type, hardware_types[sel], 31); return true; }
            }
            break;
        case 5:
            {
                int sel = dialog_select("Select Interface", interface_types, 5, 0);
                if (sel >= 0) { strncpy(form->interface, interface_types[sel], 15); return true; }
            }
            break;
        case 6:
            strncpy(buffer, form->address, 63);
            if (tui_get_string(dialog, row, col, buffer, 30, buffer)) {
                strncpy(form->address, buffer, 31);
                return true;
            }
            break;
        case 7: return tui_get_int(dialog, row, col, &form->bus, 0, 10);
        case 8: return tui_get_int(dialog, row, col, &form->channel, 0, 15);
        case 9: return tui_get_int(dialog, row, col, &form->gain, 0, 16);
        case 10: return tui_get_float(dialog, row, col, &form->reference_voltage, 0.0f, 10.0f);
        case 11:
            strncpy(buffer, form->unit, 63);
            if (tui_get_string(dialog, row, col, buffer, 15, buffer)) {
                strncpy(form->unit, buffer, 15);
                return true;
            }
            break;
        case 12: return tui_get_float(dialog, row, col, &form->min_value, -1e6f, 1e6f);
        case 13: return tui_get_float(dialog, row, col, &form->max_value, -1e6f, 1e6f);
        case 14: return tui_get_int(dialog, row, col, &form->poll_rate_ms, 10, 60000);
    }
    return false;
}

static result_t check_gpio_conflict(database_t *db, sensor_form_t *form, int exclude_sensor_id) {
    /* Check if this sensor uses GPIO interface */
    if (strcmp(form->interface, "gpio") != 0 &&
        strcmp(form->sensor_type, "DHT22") != 0 &&
        strcmp(form->sensor_type, "Float Switch") != 0) {
        return RESULT_OK;  /* Not a GPIO sensor, no conflict possible */
    }

    /* Parse GPIO pin from address field */
    int gpio_pin = atoi(form->address);
    if (gpio_pin <= 0 && strlen(form->address) > 0) {
        /* Try parsing as GPIO number without prefix */
        if (sscanf(form->address, "GPIO%d", &gpio_pin) != 1) {
            gpio_pin = atoi(form->address);
        }
    }

    if (gpio_pin <= 0) {
        return RESULT_OK;  /* No valid GPIO pin specified */
    }

    /* Use the actuator GPIO conflict check which also checks sensors */
    gpio_conflict_t conflict;
    result_t r = db_actuator_gpio_conflict_check(db, gpio_pin, "gpiochip0", 0, &conflict);
    if (r == RESULT_OK && conflict.has_conflict) {
        /* Check if the conflict is with ourselves (when editing) */
        if (exclude_sensor_id > 0 && conflict.conflict_type == 1) {
            /* conflict_type 1 = sensor - ignore if it's the same sensor */
            /* We can't easily check this without knowing the conflicting sensor ID */
        }

        char msg[256];
        snprintf(msg, sizeof(msg), "GPIO %d is already in use by %s '%s'",
                 gpio_pin,
                 conflict.conflict_type == 0 ? "actuator" : "sensor",
                 conflict.conflicting_name);
        dialog_error(msg);
        return RESULT_ALREADY_EXISTS;
    }

    return RESULT_OK;
}

static result_t save_sensor(sensor_form_t *form, int *sensor_id) {
    database_t *db = tui_get_database();
    if (!db) return RESULT_NOT_INITIALIZED;

    /* Check for GPIO conflicts before saving */
    result_t r = check_gpio_conflict(db, form, 0);
    if (r != RESULT_OK) {
        return r;
    }

    db_module_t module = {0};
    module.slot = form->slot;
    module.subslot = form->subslot;
    SAFE_STRNCPY(module.name, form->name, sizeof(module.name));
    SAFE_STRNCPY(module.module_type, form->module_type, sizeof(module.module_type));
    module.module_ident = form->module_ident;
    module.submodule_ident = form->submodule_ident;
    strcpy(module.status, STATUS_INACTIVE);

    r = db_module_create(db, &module, sensor_id);
    if (r != RESULT_OK) return r;
    
    if (strcmp(form->module_type, "physical") == 0) {
        db_physical_sensor_t phys = {0};
        phys.module_id = *sensor_id;
        SAFE_STRNCPY(phys.sensor_type, form->sensor_type, sizeof(phys.sensor_type));
        SAFE_STRNCPY(phys.hardware_type, form->sensor_type, sizeof(phys.hardware_type));
        SAFE_STRNCPY(phys.interface, form->interface, sizeof(phys.interface));
        SAFE_STRNCPY(phys.address, form->address, sizeof(phys.address));
        phys.bus = form->bus;
        phys.channel = form->channel;
        SAFE_STRNCPY(phys.unit, form->unit, sizeof(phys.unit));
        phys.min_value = form->min_value;
        phys.max_value = form->max_value;
        phys.poll_rate_ms = form->poll_rate_ms;
        db_physical_sensor_create(db, &phys);
    } else if (strcmp(form->module_type, "adc") == 0) {
        db_adc_sensor_t adc = {0};
        adc.module_id = *sensor_id;
        SAFE_STRNCPY(adc.adc_type, form->sensor_type, sizeof(adc.adc_type));
        SAFE_STRNCPY(adc.interface, form->interface, sizeof(adc.interface));
        SAFE_STRNCPY(adc.address, form->address, sizeof(adc.address));
        adc.bus = form->bus;
        adc.channel = form->channel;
        adc.gain = form->gain;
        adc.reference_voltage = form->reference_voltage;
        SAFE_STRNCPY(adc.unit, form->unit, sizeof(adc.unit));
        adc.eng_min = form->min_value;
        adc.eng_max = form->max_value;
        adc.poll_rate_ms = form->poll_rate_ms;
        db_adc_sensor_create(db, &adc);
    }
    
    return RESULT_OK;
}

int dialog_sensor_add(void) {
    WINDOW *dialog = dialog_create(DIALOG_HEIGHT, DIALOG_WIDTH, "Add Sensor");
    if (!dialog) return -1;
    
    sensor_form_t form;
    init_form(&form);
    
    int selected = 0, button = 0;
    bool in_buttons = false;
    
    while (1) {
        werase(dialog);
        box(dialog, 0, 0);
        wattron(dialog, A_BOLD);
        mvwprintw(dialog, 0, (DIALOG_WIDTH - 14) / 2, " Add Sensor ");
        wattroff(dialog, A_BOLD);
        
        draw_form(dialog, &form, in_buttons ? -1 : selected, false);
        
        const char *buttons[] = {"Save", "Cancel"};
        dialog_draw_buttons(dialog, DIALOG_HEIGHT - 2, buttons, 2, in_buttons ? button : -1);
        wrefresh(dialog);
        
        int ch = wgetch(dialog);
        
        if (in_buttons) {
            switch (ch) {
                case KEY_LEFT: button = 0; break;
                case KEY_RIGHT: button = 1; break;
                case KEY_UP: case '\t': in_buttons = false; break;
                case '\n': case KEY_ENTER:
                    if (button == 0) {
                        if (strlen(form.name) == 0) { dialog_error("Name required"); continue; }
                        int sensor_id;
                        if (save_sensor(&form, &sensor_id) == RESULT_OK) {
                            dialog_destroy(dialog);
                            return sensor_id;
                        }
                        dialog_error("Failed to save");
                    } else { dialog_destroy(dialog); return -1; }
                    break;
                case 27: dialog_destroy(dialog); return -1;
            }
        } else {
            switch (ch) {
                case KEY_UP: if (selected > 0) selected--; break;
                case KEY_DOWN: if (selected < 14) selected++; else { in_buttons = true; button = 0; } break;
                case '\t': in_buttons = true; button = 0; break;
                case '\n': case KEY_ENTER: edit_field(dialog, &form, selected); break;
                case 27: dialog_destroy(dialog); return -1;
            }
        }
    }
}

bool dialog_sensor_edit(int sensor_id) {
    database_t *db = tui_get_database();
    if (!db) return false;
    
    db_module_t module;
    if (db_module_get(db, sensor_id, &module) != RESULT_OK) {
        dialog_error("Sensor not found");
        return false;
    }
    
    sensor_form_t form;
    init_form(&form);
    SAFE_STRNCPY(form.name, module.name, sizeof(form.name));
    form.slot = module.slot;
    form.subslot = module.subslot;
    SAFE_STRNCPY(form.module_type, module.module_type, sizeof(form.module_type));
    
    db_physical_sensor_t phys;
    if (db_physical_sensor_get(db, sensor_id, &phys) == RESULT_OK) {
        SAFE_STRNCPY(form.sensor_type, phys.sensor_type, sizeof(form.sensor_type));
        SAFE_STRNCPY(form.interface, phys.interface, sizeof(form.interface));
        SAFE_STRNCPY(form.address, phys.address, sizeof(form.address));
        form.bus = phys.bus;
        form.channel = phys.channel;
        SAFE_STRNCPY(form.unit, phys.unit, sizeof(form.unit));
        form.min_value = phys.min_value;
        form.max_value = phys.max_value;
        form.poll_rate_ms = phys.poll_rate_ms;
    }
    
    WINDOW *dialog = dialog_create(DIALOG_HEIGHT, DIALOG_WIDTH, "Edit Sensor");
    if (!dialog) return false;
    
    int selected = 0, button = 0;
    bool in_buttons = false;
    
    while (1) {
        werase(dialog);
        box(dialog, 0, 0);
        wattron(dialog, A_BOLD);
        mvwprintw(dialog, 0, (DIALOG_WIDTH - 14) / 2, " Edit Sensor ");
        wattroff(dialog, A_BOLD);
        
        draw_form(dialog, &form, in_buttons ? -1 : selected, false);
        
        const char *buttons[] = {"Save", "Cancel"};
        dialog_draw_buttons(dialog, DIALOG_HEIGHT - 2, buttons, 2, in_buttons ? button : -1);
        wrefresh(dialog);
        
        int ch = wgetch(dialog);
        
        if (in_buttons) {
            switch (ch) {
                case KEY_LEFT: button = 0; break;
                case KEY_RIGHT: button = 1; break;
                case KEY_UP: case '\t': in_buttons = false; break;
                case '\n': case KEY_ENTER:
                    if (button == 0) {
                        module.slot = form.slot;
                        module.subslot = form.subslot;
                        SAFE_STRNCPY(module.name, form.name, sizeof(module.name));
                        if (db_module_update(db, &module) == RESULT_OK) {
                            dialog_destroy(dialog);
                            return true;
                        }
                        dialog_error("Failed to update");
                    } else { dialog_destroy(dialog); return false; }
                    break;
                case 27: dialog_destroy(dialog); return false;
            }
        } else {
            switch (ch) {
                case KEY_UP: if (selected > 0) selected--; break;
                case KEY_DOWN: if (selected < 14) selected++; else { in_buttons = true; button = 0; } break;
                case '\t': in_buttons = true; button = 0; break;
                case '\n': case KEY_ENTER: edit_field(dialog, &form, selected); break;
                case 27: dialog_destroy(dialog); return false;
            }
        }
    }
}

bool dialog_sensor_delete(int sensor_id) {
    database_t *db = tui_get_database();
    if (!db) return false;
    
    db_module_t module;
    if (db_module_get(db, sensor_id, &module) != RESULT_OK) {
        dialog_error("Sensor not found");
        return false;
    }
    
    char msg[128];
    snprintf(msg, sizeof(msg), "Delete '%s' (slot %d)?", module.name, module.slot);
    
    if (dialog_confirm("Confirm Delete", msg)) {
        if (db_module_delete(db, sensor_id) == RESULT_OK) return true;
        dialog_error("Failed to delete");
    }
    return false;
}
