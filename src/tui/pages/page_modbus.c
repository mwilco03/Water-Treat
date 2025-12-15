/**
 * @file page_modbus.c
 * @brief Modbus gateway configuration page
 */

#include "page_modbus.h"
#include "../tui_common.h"
#include "config/config.h"
#include "db/db_modules.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>

#define MAX_FIELDS 15
#define MAX_REGISTERS 64

typedef struct {
    const char *label;
    char value[64];
    bool editable;
    const char *config_key;
} field_t;

typedef struct {
    int address;
    int module_id;
    char name[32];
    char type[16];  // holding, input, coil, discrete
    float value;
} register_map_t;

static struct {
    WINDOW *win;
    
    field_t fields[MAX_FIELDS];
    int field_count;
    int selected_field;
    
    register_map_t registers[MAX_REGISTERS];
    int register_count;
    int selected_register;
    
    bool editing;
    char edit_buffer[64];
    int edit_pos;
    
    int view_mode;  // 0 = config, 1 = register map
    bool modbus_enabled;
} g_page = {0};

static void load_modbus_config(void) {
    g_page.field_count = 0;
    
    config_manager_t *cfg_mgr = tui_get_config_manager();
    
    field_t *f;
    char value[64];
    
    // Enabled
    f = &g_page.fields[g_page.field_count++];
    f->label = "Modbus Enabled";
    f->editable = true;
    f->config_key = "enabled";
    if (cfg_mgr && config_get_string(cfg_mgr, "modbus", "enabled", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
        g_page.modbus_enabled = (strcmp(value, "true") == 0);
    } else {
        strcpy(f->value, "false");
        g_page.modbus_enabled = false;
    }
    
    // TCP Port
    f = &g_page.fields[g_page.field_count++];
    f->label = "TCP Port";
    f->editable = true;
    f->config_key = "port";
    if (cfg_mgr && config_get_string(cfg_mgr, "modbus", "port", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "502");
    }
    
    // Unit ID
    f = &g_page.fields[g_page.field_count++];
    f->label = "Unit ID";
    f->editable = true;
    f->config_key = "unit_id";
    if (cfg_mgr && config_get_string(cfg_mgr, "modbus", "unit_id", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "1");
    }
    
    // Max Connections
    f = &g_page.fields[g_page.field_count++];
    f->label = "Max Connections";
    f->editable = true;
    f->config_key = "max_connections";
    if (cfg_mgr && config_get_string(cfg_mgr, "modbus", "max_connections", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "5");
    }
    
    // Timeout
    f = &g_page.fields[g_page.field_count++];
    f->label = "Timeout (ms)";
    f->editable = true;
    f->config_key = "timeout_ms";
    if (cfg_mgr && config_get_string(cfg_mgr, "modbus", "timeout_ms", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "1000");
    }
    
    // RTU Enabled
    f = &g_page.fields[g_page.field_count++];
    f->label = "RTU Enabled";
    f->editable = true;
    f->config_key = "rtu_enabled";
    if (cfg_mgr && config_get_string(cfg_mgr, "modbus", "rtu_enabled", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "false");
    }
    
    // Serial Port
    f = &g_page.fields[g_page.field_count++];
    f->label = "Serial Port";
    f->editable = true;
    f->config_key = "serial_port";
    if (cfg_mgr && config_get_string(cfg_mgr, "modbus", "serial_port", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "/dev/ttyUSB0");
    }
    
    // Baud Rate
    f = &g_page.fields[g_page.field_count++];
    f->label = "Baud Rate";
    f->editable = true;
    f->config_key = "baud_rate";
    if (cfg_mgr && config_get_string(cfg_mgr, "modbus", "baud_rate", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "9600");
    }
    
    // Data Bits
    f = &g_page.fields[g_page.field_count++];
    f->label = "Data Bits";
    f->editable = true;
    f->config_key = "data_bits";
    if (cfg_mgr && config_get_string(cfg_mgr, "modbus", "data_bits", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "8");
    }
    
    // Parity
    f = &g_page.fields[g_page.field_count++];
    f->label = "Parity";
    f->editable = true;
    f->config_key = "parity";
    if (cfg_mgr && config_get_string(cfg_mgr, "modbus", "parity", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "N");
    }
    
    // Stop Bits
    f = &g_page.fields[g_page.field_count++];
    f->label = "Stop Bits";
    f->editable = true;
    f->config_key = "stop_bits";
    if (cfg_mgr && config_get_string(cfg_mgr, "modbus", "stop_bits", value, sizeof(value)) == RESULT_OK) {
        SAFE_STRNCPY(f->value, value, sizeof(f->value));
    } else {
        strcpy(f->value, "1");
    }
}

static void load_register_map(void) {
    g_page.register_count = 0;
    
    // Auto-generate register map from sensors
    database_t *db = tui_get_database();
    if (!db) return;
    
    db_module_t *modules = NULL;
    int count = 0;
    
    if (db_module_list(db, &modules, &count) != RESULT_OK || !modules) return;
    
    for (int i = 0; i < count && g_page.register_count < MAX_REGISTERS; i++) {
        register_map_t *reg = &g_page.registers[g_page.register_count];
        
        reg->address = 40001 + (i * 2);  // Holding registers, 2 per sensor (float)
        reg->module_id = modules[i].id;
        SAFE_STRNCPY(reg->name, modules[i].name, sizeof(reg->name));
        strcpy(reg->type, "holding");
        
        // Get current value
        float value;
        char status[16];
        if (db_sensor_status_get(db, modules[i].id, &value, status, sizeof(status)) == RESULT_OK) {
            reg->value = value;
        }
        
        g_page.register_count++;
    }
    
    free(modules);
}

static void save_field(int idx) {
    if (idx < 0 || idx >= g_page.field_count) return;
    
    field_t *f = &g_page.fields[idx];
    if (!f->editable) return;
    
    config_manager_t *cfg_mgr = tui_get_config_manager();
    if (cfg_mgr) {
        config_set_string(cfg_mgr, "modbus", f->config_key, f->value);
        tui_set_status("Saved: %s", f->label);
        
        if (strcmp(f->config_key, "enabled") == 0) {
            g_page.modbus_enabled = (strcmp(f->value, "true") == 0);
        }
    }
}

static void draw_config(WINDOW *win) {
    int row = 3;
    int max_x = getmaxx(win);
    
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, row++, 2, "Modbus Gateway Configuration");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    row++;
    
    // Status
    wattron(win, COLOR_PAIR(g_page.modbus_enabled ? TUI_COLOR_STATUS : TUI_COLOR_WARNING));
    mvwprintw(win, row++, 4, "Status: %s", g_page.modbus_enabled ? "ENABLED" : "DISABLED");
    wattroff(win, COLOR_PAIR(g_page.modbus_enabled ? TUI_COLOR_STATUS : TUI_COLOR_WARNING));
    row++;
    
    // TCP Settings header
    wattron(win, A_UNDERLINE);
    mvwprintw(win, row++, 4, "TCP/IP Settings");
    wattroff(win, A_UNDERLINE);
    
    // Draw TCP fields (first 5)
    for (int i = 0; i < MIN(5, g_page.field_count); i++) {
        field_t *f = &g_page.fields[i];
        
        if (i == g_page.selected_field && g_page.view_mode == 0) {
            wattron(win, A_REVERSE);
        }
        
        mvwprintw(win, row, 6, "%-16s: ", f->label);
        
        if (g_page.editing && i == g_page.selected_field) {
            wattron(win, COLOR_PAIR(TUI_COLOR_INPUT));
            wprintw(win, "%-20s", g_page.edit_buffer);
            wattroff(win, COLOR_PAIR(TUI_COLOR_INPUT));
        } else {
            wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
            wprintw(win, "%-20s", f->value);
            wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
        }
        
        if (i == g_page.selected_field && g_page.view_mode == 0) {
            wattroff(win, A_REVERSE);
        }
        
        row++;
    }
    
    row++;
    
    // RTU Settings header
    wattron(win, A_UNDERLINE);
    mvwprintw(win, row++, 4, "Serial/RTU Settings");
    wattroff(win, A_UNDERLINE);
    
    // Draw RTU fields (remaining)
    for (int i = 5; i < g_page.field_count; i++) {
        field_t *f = &g_page.fields[i];
        
        if (i == g_page.selected_field && g_page.view_mode == 0) {
            wattron(win, A_REVERSE);
        }
        
        mvwprintw(win, row, 6, "%-16s: ", f->label);
        
        if (g_page.editing && i == g_page.selected_field) {
            wattron(win, COLOR_PAIR(TUI_COLOR_INPUT));
            wprintw(win, "%-20s", g_page.edit_buffer);
            wattroff(win, COLOR_PAIR(TUI_COLOR_INPUT));
        } else {
            wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
            wprintw(win, "%-20s", f->value);
            wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
        }
        
        if (i == g_page.selected_field && g_page.view_mode == 0) {
            wattroff(win, A_REVERSE);
        }
        
        row++;
    }
    UNUSED(max_x);
}

static void draw_register_map(WINDOW *win) {
    int row = 3;
    int start_col = 50;
    
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, row++, start_col, "Register Map");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    row++;
    
    wattron(win, A_BOLD);
    mvwprintw(win, row++, start_col, "%-8s %-20s %-10s",
              "Address", "Sensor", "Value");
    wattroff(win, A_BOLD);
    
    mvwhline(win, row++, start_col, ACS_HLINE, 40);
    
    int visible = MIN(12, g_page.register_count);
    
    for (int i = 0; i < visible; i++) {
        register_map_t *reg = &g_page.registers[i];
        
        if (i == g_page.selected_register && g_page.view_mode == 1) {
            wattron(win, A_REVERSE);
        }
        
        mvwprintw(win, row, start_col, "%-8d %-20s ", reg->address, reg->name);
        wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
        wprintw(win, "%-10.2f", reg->value);
        wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
        
        if (i == g_page.selected_register && g_page.view_mode == 1) {
            wattroff(win, A_REVERSE);
        }
        
        row++;
    }
    
    if (g_page.register_count == 0) {
        wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
        mvwprintw(win, row, start_col, "No registers mapped");
        wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
    }
}

static void draw_help(WINDOW *win) {
    int max_y = getmaxy(win);
    int row = max_y - 3;
    
    mvwhline(win, row++, 2, ACS_HLINE, getmaxx(win) - 4);
    wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwprintw(win, row++, 2, "Tab:Switch view  Up/Down:Select  Enter:Edit  r:Refresh  Ctrl+S:Save");
    wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
}

void page_modbus_init(WINDOW *win) {
    g_page.win = win;
    g_page.view_mode = 0;
    g_page.selected_field = 0;
    g_page.selected_register = 0;
    g_page.editing = false;
    load_modbus_config();
    load_register_map();
}

void page_modbus_draw(WINDOW *win) {
    draw_config(win);
    draw_register_map(win);
    draw_help(win);
}

void page_modbus_input(WINDOW *win, int ch) {
    UNUSED(win);
    
    if (g_page.editing) {
        switch (ch) {
            case 27:
                g_page.editing = false;
                break;
            case '\n':
            case KEY_ENTER:
                SAFE_STRNCPY(g_page.fields[g_page.selected_field].value,
                            g_page.edit_buffer,
                            sizeof(g_page.fields[g_page.selected_field].value));
                save_field(g_page.selected_field);
                g_page.editing = false;
                break;
            case KEY_BACKSPACE:
            case 127:
                if (g_page.edit_pos > 0) {
                    g_page.edit_buffer[--g_page.edit_pos] = '\0';
                }
                break;
            default:
                if (ch >= 32 && ch < 127 && g_page.edit_pos < (int)sizeof(g_page.edit_buffer) - 1) {
                    g_page.edit_buffer[g_page.edit_pos++] = ch;
                    g_page.edit_buffer[g_page.edit_pos] = '\0';
                }
                break;
        }
        return;
    }
    
    switch (ch) {
        case '\t':
            g_page.view_mode = (g_page.view_mode + 1) % 2;
            break;
            
        case KEY_UP:
            if (g_page.view_mode == 0) {
                if (g_page.selected_field > 0) g_page.selected_field--;
            } else {
                if (g_page.selected_register > 0) g_page.selected_register--;
            }
            break;
            
        case KEY_DOWN:
            if (g_page.view_mode == 0) {
                if (g_page.selected_field < g_page.field_count - 1) g_page.selected_field++;
            } else {
                if (g_page.selected_register < g_page.register_count - 1) g_page.selected_register++;
            }
            break;
            
        case '\n':
        case KEY_ENTER:
            if (g_page.view_mode == 0 && g_page.fields[g_page.selected_field].editable) {
                g_page.editing = true;
                SAFE_STRNCPY(g_page.edit_buffer,
                            g_page.fields[g_page.selected_field].value,
                            sizeof(g_page.edit_buffer));
                g_page.edit_pos = strlen(g_page.edit_buffer);
            }
            break;
            
        case 'r':
        case 'R':
            load_modbus_config();
            load_register_map();
            tui_set_status("Refreshed");
            break;
            
        case 19:  // Ctrl+S
            {
                config_manager_t *cfg = tui_get_config_manager();
                if (cfg) {
                    config_save_file(cfg, NULL);
                    tui_set_status("Configuration saved");
                }
            }
            break;
    }
}

void page_modbus_cleanup(void) {
    g_page.win = NULL;
}
