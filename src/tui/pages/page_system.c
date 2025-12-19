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
#include <dirent.h>

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
    mvwprintw(win, row++, label_col, "Navigation: Up/Down arrows | Edit: Enter");
    mvwprintw(win, row++, label_col, "Save config: Ctrl+S | Export: E | Import: I");
    mvwprintw(win, row++, label_col, "Database backup: B | Database restore: D");
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

#define MAX_BACKUP_FILES 20

typedef struct {
    char name[64];
    char path[256];
    time_t mtime;
} backup_file_t;

static int scan_backup_files(backup_file_t *files, int max_files, const char *extension) {
    const char *backup_dir = "/var/backup/profinet-monitor";
    DIR *dir = opendir(backup_dir);
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    struct stat st;

    while ((entry = readdir(dir)) != NULL && count < max_files) {
        if (entry->d_type != DT_REG) continue;

        /* Check extension */
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext + 1, extension) != 0) continue;

        /* Build full path and get mtime */
        snprintf(files[count].path, sizeof(files[count].path), "%s/%s", backup_dir, entry->d_name);
        if (stat(files[count].path, &st) == 0) {
            SAFE_STRNCPY(files[count].name, entry->d_name, sizeof(files[count].name));
            files[count].mtime = st.st_mtime;
            count++;
        }
    }

    closedir(dir);

    /* Sort by mtime descending (newest first) */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (files[j].mtime > files[i].mtime) {
                backup_file_t tmp = files[i];
                files[i] = files[j];
                files[j] = tmp;
            }
        }
    }

    return count;
}

static int file_browser_dialog(const char *title, backup_file_t *files, int file_count) {
    if (file_count == 0) return -1;

    int dialog_height = MIN(file_count + 6, 18);
    int dialog_width = 60;
    WINDOW *dialog = newwin(dialog_height, dialog_width, 5, 10);
    box(dialog, 0, 0);

    wattron(dialog, A_BOLD);
    mvwprintw(dialog, 0, (dialog_width - strlen(title) - 4) / 2, " %s ", title);
    wattroff(dialog, A_BOLD);

    int selected = 0;
    int visible = MIN(file_count, dialog_height - 5);

    keypad(dialog, TRUE);
    nodelay(dialog, FALSE);

    while (1) {
        /* Draw file list */
        for (int i = 0; i < visible; i++) {
            backup_file_t *f = &files[i];
            if (i == selected) wattron(dialog, A_REVERSE);

            /* Format time */
            struct tm *tm = localtime(&f->mtime);
            char timestr[32];
            strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M", tm);

            mvwprintw(dialog, 2 + i, 2, "%-32s %s", f->name, timestr);

            if (i == selected) wattroff(dialog, A_REVERSE);
        }

        mvwprintw(dialog, dialog_height - 2, 2, "Enter:Select  Esc:Cancel");
        wrefresh(dialog);

        int ch = wgetch(dialog);
        switch (ch) {
            case KEY_UP:
                if (selected > 0) selected--;
                break;
            case KEY_DOWN:
                if (selected < visible - 1) selected++;
                break;
            case '\n':
            case KEY_ENTER:
                delwin(dialog);
                return selected;
            case 27:  /* Escape */
                delwin(dialog);
                return -1;
        }
    }
}

static void import_config(void) {
    backup_file_t files[MAX_BACKUP_FILES];
    int count = scan_backup_files(files, MAX_BACKUP_FILES, "conf");

    if (count == 0) {
        tui_set_status("No config files found in /var/backup/profinet-monitor/");
        return;
    }

    int sel = file_browser_dialog("Select Config File to Import", files, count);
    if (sel < 0) {
        tui_set_status("Import cancelled");
        return;
    }

    const char *import_path = files[sel].path;

    config_manager_t *cfg = tui_get_config_manager();
    if (cfg && config_load_file(cfg, import_path) == RESULT_OK) {
        /* Reload app config */
        app_config_t *app_cfg = tui_get_app_config();
        if (app_cfg) {
            config_load_app_config(cfg, app_cfg);
        }
        load_system_info();
        tui_set_status("Config imported from %s", files[sel].name);
        LOG_INFO("Configuration imported from %s", import_path);
    } else {
        tui_set_status("Import failed!");
    }
}

static bool copy_file(const char *src, const char *dst) {
    FILE *src_fp = fopen(src, "rb");
    if (!src_fp) return false;

    FILE *dst_fp = fopen(dst, "wb");
    if (!dst_fp) {
        fclose(src_fp);
        return false;
    }

    char buf[8192];
    size_t n;
    bool success = true;

    while ((n = fread(buf, 1, sizeof(buf), src_fp)) > 0) {
        if (fwrite(buf, 1, n, dst_fp) != n) {
            success = false;
            break;
        }
    }

    fclose(src_fp);
    fclose(dst_fp);
    return success;
}

static void backup_database(void) {
    app_config_t *cfg = tui_get_app_config();
    if (!cfg || strlen(cfg->database.path) == 0) {
        tui_set_status("Database path not configured");
        return;
    }

    if (access(cfg->database.path, R_OK) != 0) {
        tui_set_status("Database file not found: %s", cfg->database.path);
        return;
    }

    /* Create backup directory */
    mkdir("/var/backup", 0755);
    mkdir("/var/backup/profinet-monitor", 0755);

    /* Generate timestamped filename */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char filename[256];
    snprintf(filename, sizeof(filename),
             "/var/backup/profinet-monitor/database_%04d%02d%02d_%02d%02d%02d.db",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    /* Copy the database file */
    if (copy_file(cfg->database.path, filename)) {
        tui_set_status("Database backed up to %s", filename);
        LOG_INFO("Database backed up to %s", filename);
    } else {
        tui_set_status("Database backup failed!");
        LOG_ERROR("Failed to backup database to %s", filename);
    }
}

static void restore_database(void) {
    backup_file_t files[MAX_BACKUP_FILES];
    int count = scan_backup_files(files, MAX_BACKUP_FILES, "db");

    if (count == 0) {
        tui_set_status("No database backups found in /var/backup/profinet-monitor/");
        return;
    }

    int sel = file_browser_dialog("Select Database Backup to Restore", files, count);
    if (sel < 0) {
        tui_set_status("Restore cancelled");
        return;
    }

    const char *restore_path = files[sel].path;

    app_config_t *cfg = tui_get_app_config();
    if (!cfg || strlen(cfg->database.path) == 0) {
        tui_set_status("Database path not configured");
        return;
    }

    /* Confirm restore */
    WINDOW *dialog = newwin(8, 60, 10, 10);
    box(dialog, 0, 0);

    wattron(dialog, A_BOLD | COLOR_PAIR(TUI_COLOR_WARNING));
    mvwprintw(dialog, 0, 18, " RESTORE DATABASE ");
    wattroff(dialog, A_BOLD | COLOR_PAIR(TUI_COLOR_WARNING));

    mvwprintw(dialog, 2, 3, "This will replace the current database!");
    mvwprintw(dialog, 3, 3, "File: %s", files[sel].name);
    mvwprintw(dialog, 4, 3, "To:   %s", cfg->database.path);
    mvwprintw(dialog, 6, 3, "Press 'Y' to confirm, any other key to cancel");
    wrefresh(dialog);

    int ch = wgetch(dialog);
    delwin(dialog);

    if (ch != 'Y' && ch != 'y') {
        tui_set_status("Restore cancelled");
        return;
    }

    /* Backup current database first */
    char backup_path[256];
    snprintf(backup_path, sizeof(backup_path), "%s.bak", cfg->database.path);
    copy_file(cfg->database.path, backup_path);

    /* Copy restore file to database path */
    if (copy_file(restore_path, cfg->database.path)) {
        tui_set_status("Database restored from %s", restore_path);
        LOG_WARNING("Database restored from %s (backup at %s)", restore_path, backup_path);
    } else {
        tui_set_status("Database restore failed!");
        LOG_ERROR("Failed to restore database from %s", restore_path);
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
            case 'b':
            case 'B':
                backup_database();
                break;
            case 'd':
            case 'D':
                restore_database();
                break;
        }
    }
}

void page_system_cleanup(void) {
    g_page.win = NULL;
}
