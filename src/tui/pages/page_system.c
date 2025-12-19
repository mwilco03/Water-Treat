/**
 * @file page_system.c
 * @brief System configuration page
 */

#include "page_system.h"
#include "../tui_common.h"
#include "config/config.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_FIELDS 10

typedef struct {
    const char *label;
    char value[128];
    bool editable;
    const char *config_section;
    const char *config_key;
} field_t;

static struct {
    WINDOW *win;
    field_t fields[MAX_FIELDS];
    int field_count;
    int selected;
    bool editing;
    char edit_buffer[128];
    int edit_pos;
} g_page = {0};

static void load_system_info(void) {
    g_page.field_count = 0;
    
    // Device Name
    field_t *f = &g_page.fields[g_page.field_count++];
    f->label = "Device Name";
    f->editable = true;
    f->config_section = "system";
    f->config_key = "device_name";
    
    app_config_t *cfg = tui_get_app_config();
    if (cfg) {
        SAFE_STRNCPY(f->value, cfg->system.device_name, sizeof(f->value));
    } else {
        strcpy(f->value, "profinet-monitor");
    }
    
    // Log Level
    f = &g_page.fields[g_page.field_count++];
    f->label = "Log Level";
    f->editable = true;
    f->config_section = "system";
    f->config_key = "log_level";
    if (cfg) {
        SAFE_STRNCPY(f->value, cfg->system.log_level, sizeof(f->value));
    } else {
        strcpy(f->value, "info");
    }
    
    // Hostname
    f = &g_page.fields[g_page.field_count++];
    f->label = "Hostname";
    f->editable = false;
    struct utsname uts;
    if (uname(&uts) == 0) {
        SAFE_STRNCPY(f->value, uts.nodename, sizeof(f->value));
    } else {
        strcpy(f->value, "unknown");
    }
    
    // Kernel Version
    f = &g_page.fields[g_page.field_count++];
    f->label = "Kernel";
    f->editable = false;
    if (uname(&uts) == 0) {
        snprintf(f->value, sizeof(f->value), "%s %s", uts.sysname, uts.release);
    }
    
    // Uptime
    f = &g_page.fields[g_page.field_count++];
    f->label = "Uptime";
    f->editable = false;
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        int days = si.uptime / 86400;
        int hours = (si.uptime % 86400) / 3600;
        int mins = (si.uptime % 3600) / 60;
        snprintf(f->value, sizeof(f->value), "%dd %dh %dm", days, hours, mins);
    }
    
    // Memory
    f = &g_page.fields[g_page.field_count++];
    f->label = "Memory";
    f->editable = false;
    if (sysinfo(&si) == 0) {
        unsigned long total_mb = si.totalram / (1024 * 1024);
        unsigned long free_mb = si.freeram / (1024 * 1024);
        unsigned long used_mb = total_mb - free_mb;
        snprintf(f->value, sizeof(f->value), "%lu / %lu MB (%.1f%%)", 
                 used_mb, total_mb, (float)used_mb / total_mb * 100);
    }
    
    // CPU Temperature (Raspberry Pi)
    f = &g_page.fields[g_page.field_count++];
    f->label = "CPU Temp";
    f->editable = false;
    FILE *fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (fp) {
        int temp;
        if (fscanf(fp, "%d", &temp) == 1) {
            snprintf(f->value, sizeof(f->value), "%.1f C", temp / 1000.0);
        }
        fclose(fp);
    } else {
        strcpy(f->value, "N/A");
    }
    
    // Database Path
    f = &g_page.fields[g_page.field_count++];
    f->label = "Database";
    f->editable = false;
    if (cfg) {
        SAFE_STRNCPY(f->value, cfg->database.path, sizeof(f->value));
    }
}

static void save_field(int idx) {
    if (idx < 0 || idx >= g_page.field_count) return;
    
    field_t *f = &g_page.fields[idx];
    if (!f->editable || !f->config_section || !f->config_key) return;
    
    config_manager_t *cfg_mgr = tui_get_config_manager();
    if (cfg_mgr) {
        config_set_string(cfg_mgr, f->config_section, f->config_key, f->value);
        
        // Update app config
        app_config_t *app_cfg = tui_get_app_config();
        if (app_cfg) {
            if (strcmp(f->config_key, "device_name") == 0) {
                SAFE_STRNCPY(app_cfg->system.device_name, f->value, sizeof(app_cfg->system.device_name));
            } else if (strcmp(f->config_key, "log_level") == 0) {
                SAFE_STRNCPY(app_cfg->system.log_level, f->value, sizeof(app_cfg->system.log_level));
                logger_set_level(log_level_from_string(f->value));
            }
        }
        
        tui_set_status("Saved: %s", f->label);
    }
}

void page_system_init(WINDOW *win) {
    g_page.win = win;
    g_page.selected = 0;
    g_page.editing = false;
    load_system_info();
}

void page_system_draw(WINDOW *win) {
    int row = 2;
    int label_col = 4;
    int value_col = 20;
    int max_x = getmaxx(win);
    
    // Section header
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, row++, label_col, "System Information");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    row++;
    
    // Draw fields
    for (int i = 0; i < g_page.field_count; i++) {
        field_t *f = &g_page.fields[i];
        
        // Highlight selected
        if (i == g_page.selected) {
            wattron(win, A_REVERSE);
        }
        
        // Label
        mvwprintw(win, row, label_col, "%-14s:", f->label);
        
        // Value
        if (g_page.editing && i == g_page.selected) {
            wattron(win, COLOR_PAIR(TUI_COLOR_INPUT));
            mvwprintw(win, row, value_col, "%-40s", g_page.edit_buffer);
            wattroff(win, COLOR_PAIR(TUI_COLOR_INPUT));
        } else {
            if (f->editable) {
                wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
            }
            mvwprintw(win, row, value_col, "%-40s", f->value);
            if (f->editable) {
                wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
            }
        }
        
        // Editable indicator
        if (f->editable && !g_page.editing) {
            mvwprintw(win, row, max_x - 10, "[Edit]");
        }
        
        if (i == g_page.selected) {
            wattroff(win, A_REVERSE);
        }
        
        row++;
    }
    
    // Help text
    row += 2;
    wattron(win, COLOR_PAIR(TUI_COLOR_NORMAL));
    mvwprintw(win, row++, label_col, "Navigation: Up/Down arrows");
    mvwprintw(win, row++, label_col, "Edit: Enter to edit, Enter to save, Esc to cancel");
    mvwprintw(win, row++, label_col, "Save config: Ctrl+S | Export: E | Import: I");
    wattroff(win, COLOR_PAIR(TUI_COLOR_NORMAL));
}

static void export_config(void) {
    /* Create backup directory if it doesn't exist */
    mkdir("/var/backup", 0755);
    mkdir("/var/backup/profinet-monitor", 0755);

    /* Generate timestamped filename */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char filename[256];
    snprintf(filename, sizeof(filename),
             "/var/backup/profinet-monitor/config_%04d%02d%02d_%02d%02d%02d.conf",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    config_manager_t *cfg = tui_get_config_manager();
    if (cfg && config_save_file(cfg, filename) == RESULT_OK) {
        tui_set_status("Config exported to %s", filename);
        LOG_INFO("Configuration exported to %s", filename);
    } else {
        tui_set_status("Export failed!");
    }
}

static void import_config(void) {
    /* Show file browser dialog - for now just import from fixed path */
    const char *import_path = "/var/backup/profinet-monitor/import.conf";

    if (access(import_path, R_OK) != 0) {
        tui_set_status("No import file found at %s", import_path);
        return;
    }

    config_manager_t *cfg = tui_get_config_manager();
    if (cfg && config_load_file(cfg, import_path) == RESULT_OK) {
        /* Reload app config */
        app_config_t *app_cfg = tui_get_app_config();
        if (app_cfg) {
            config_load_app_config(cfg, app_cfg);
        }
        load_system_info();
        tui_set_status("Config imported from %s", import_path);
        LOG_INFO("Configuration imported from %s", import_path);
    } else {
        tui_set_status("Import failed!");
    }
}

void page_system_input(WINDOW *win, int ch) {
    UNUSED(win);

    if (g_page.editing) {
        switch (ch) {
            case 27:  // Escape
                g_page.editing = false;
                break;
            case '\n':
            case KEY_ENTER:
                // Save edit
                SAFE_STRNCPY(g_page.fields[g_page.selected].value, 
                            g_page.edit_buffer, 
                            sizeof(g_page.fields[g_page.selected].value));
                save_field(g_page.selected);
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
    } else {
        switch (ch) {
            case KEY_UP:
                if (g_page.selected > 0) g_page.selected--;
                break;
            case KEY_DOWN:
                if (g_page.selected < g_page.field_count - 1) g_page.selected++;
                break;
            case '\n':
            case KEY_ENTER:
                if (g_page.fields[g_page.selected].editable) {
                    g_page.editing = true;
                    SAFE_STRNCPY(g_page.edit_buffer, 
                                g_page.fields[g_page.selected].value,
                                sizeof(g_page.edit_buffer));
                    g_page.edit_pos = strlen(g_page.edit_buffer);
                }
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
            case 'r':
            case 'R':
                load_system_info();
                tui_set_status("Refreshed");
                break;
            case 'e':
            case 'E':
                export_config();
                break;
            case 'i':
            case 'I':
                import_config();
                break;
        }
    }
}

void page_system_cleanup(void) {
    g_page.win = NULL;
}
