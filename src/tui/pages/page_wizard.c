/**
 * @file page_wizard.c
 * @brief First-run setup wizard implementation
 */

#include "page_wizard.h"
#include "tui/tui_common.h"
#include "config/config.h"
#include "config/config_validate.h"
#include "platform/board_detect.h"
#include "platform/hw_discover.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>
#include <unistd.h>

/* Wizard steps */
typedef enum {
    WIZARD_STEP_WELCOME = 0,
    WIZARD_STEP_BOARD_DETECT,
    WIZARD_STEP_NETWORK,
    WIZARD_STEP_PROFINET,
    WIZARD_STEP_SENSORS,
    WIZARD_STEP_ACTUATORS,
    WIZARD_STEP_CONFIRM,
    WIZARD_STEP_COMPLETE,
    WIZARD_STEP_COUNT
} wizard_step_t;

/* Wizard state */
static struct {
    wizard_step_t current_step;
    bool initialized;
    bool completed;
    bool cancelled;

    /* Collected configuration */
    char device_name[MAX_NAME_LEN];
    char station_name[MAX_NAME_LEN];
    char network_interface[32];
    bool use_dhcp;
    char ip_address[16];
    char netmask[16];
    char gateway[16];

    /* Detected board info */
    board_type_t board_type;
    char board_name[64];
    bool board_detected;

    /* Hardware discovery results */
    hw_discovery_result_t hw_discovery;
    bool hw_scan_done;
    bool hw_scanning;
    int hw_scroll_offset;

    /* Current field being edited */
    int current_field;
    char edit_buffer[256];
    bool editing;

} g_wizard = {0};

/* External references */
extern config_manager_t g_config_mgr;
extern app_config_t g_app_config;

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

static void wizard_draw_box(int y, int x, int h, int w, const char *title) {
    /* Draw border */
    mvhline(y, x, ACS_HLINE, w);
    mvhline(y + h - 1, x, ACS_HLINE, w);
    mvvline(y, x, ACS_VLINE, h);
    mvvline(y, x + w - 1, ACS_VLINE, h);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);

    /* Draw title */
    if (title) {
        int title_x = x + (w - strlen(title) - 4) / 2;
        mvprintw(y, title_x, "[ %s ]", title);
    }
}

static void wizard_draw_progress(int y) {
    int total = WIZARD_STEP_COUNT - 1;  /* Exclude COMPLETE */
    int current = g_wizard.current_step;
    if (current >= total) current = total - 1;

    mvprintw(y, 2, "Step %d of %d: ", current + 1, total);

    const char *step_names[] = {
        "Welcome", "Hardware", "Network", "PROFINET",
        "Sensors", "Actuators", "Confirm"
    };

    if (current < (int)(sizeof(step_names)/sizeof(step_names[0]))) {
        printw("%s", step_names[current]);
    }

    /* Progress bar */
    int bar_width = 40;
    int filled = (bar_width * current) / (total - 1);
    mvprintw(y, 50, "[");
    for (int i = 0; i < bar_width; i++) {
        addch(i < filled ? '=' : ' ');
    }
    printw("]");
}

static void wizard_draw_field(int y, int x, const char *label, const char *value,
                              bool selected, bool editing) {
    if (selected) attron(A_REVERSE);
    mvprintw(y, x, "%-20s: ", label);
    if (selected) attroff(A_REVERSE);

    if (editing) {
        attron(A_UNDERLINE);
        printw("%-30s", g_wizard.edit_buffer);
        attroff(A_UNDERLINE);
        printw("_");
    } else {
        printw("%-30s", value);
    }
}

static void wizard_draw_checkbox(int y, int x, const char *label, bool checked, bool selected) {
    if (selected) attron(A_REVERSE);
    mvprintw(y, x, "[%c] %s", checked ? 'X' : ' ', label);
    if (selected) attroff(A_REVERSE);
}

/* ============================================================================
 * Step Drawing Functions
 * ========================================================================== */

static void draw_step_welcome(void) {
    int row = 6;
    mvprintw(row++, 4, "Welcome to the Water Treatment RTU Setup Wizard");
    row++;
    mvprintw(row++, 4, "This wizard will guide you through the initial configuration:");
    row++;
    mvprintw(row++, 6, "1. Hardware Detection  - Identify your board and GPIO pins");
    mvprintw(row++, 6, "2. Network Setup       - Configure network interface");
    mvprintw(row++, 6, "3. PROFINET Config     - Set station name and parameters");
    mvprintw(row++, 6, "4. Sensor Setup        - Configure connected sensors");
    mvprintw(row++, 6, "5. Actuator Setup      - Configure pumps and valves");
    row++;
    mvprintw(row++, 4, "You can skip any step and configure it later via the TUI.");
    row += 2;

    attron(A_BOLD);
    mvprintw(row, 4, "Press ENTER to begin, or ESC to skip wizard");
    attroff(A_BOLD);
}

static void draw_step_board_detect(void) {
    int row = 6;
    mvprintw(row++, 4, "Hardware Detection");
    row++;

    if (g_wizard.board_detected) {
        attron(COLOR_PAIR(2));  /* Green */
        mvprintw(row++, 4, "Detected: %s", g_wizard.board_name);
        attroff(COLOR_PAIR(2));
        row++;

        mvprintw(row++, 4, "Default pin assignments will be used for this board.");
        mvprintw(row++, 4, "You can customize pin mappings in the Sensors/Actuators pages.");
    } else {
        attron(COLOR_PAIR(3));  /* Yellow/warning */
        mvprintw(row++, 4, "Board auto-detection inconclusive");
        attroff(COLOR_PAIR(3));
        row++;
        mvprintw(row++, 4, "Generic GPIO configuration will be used.");
        mvprintw(row++, 4, "You may need to manually configure pin mappings.");
    }

    row += 2;
    mvprintw(row++, 4, "Press ENTER to continue, or 'R' to re-detect");
}

static void draw_step_network(void) {
    int row = 6;
    mvprintw(row++, 4, "Network Configuration");
    row++;

    wizard_draw_field(row++, 4, "Interface", g_wizard.network_interface,
                      g_wizard.current_field == 0, g_wizard.editing && g_wizard.current_field == 0);
    row++;

    wizard_draw_checkbox(row++, 4, "Use DHCP (automatic IP)", g_wizard.use_dhcp,
                         g_wizard.current_field == 1);
    row++;

    if (!g_wizard.use_dhcp) {
        wizard_draw_field(row++, 4, "IP Address", g_wizard.ip_address,
                          g_wizard.current_field == 2, g_wizard.editing && g_wizard.current_field == 2);
        wizard_draw_field(row++, 4, "Netmask", g_wizard.netmask,
                          g_wizard.current_field == 3, g_wizard.editing && g_wizard.current_field == 3);
        wizard_draw_field(row++, 4, "Gateway", g_wizard.gateway,
                          g_wizard.current_field == 4, g_wizard.editing && g_wizard.current_field == 4);
    }

    row += 2;
    mvprintw(row, 4, "Use UP/DOWN to navigate, ENTER to edit, SPACE to toggle");
}

static void draw_step_profinet(void) {
    int row = 6;
    mvprintw(row++, 4, "PROFINET Configuration");
    row++;
    mvprintw(row++, 4, "These settings must match your PLC/controller configuration.");
    row++;

    wizard_draw_field(row++, 4, "Device Name", g_wizard.device_name,
                      g_wizard.current_field == 0, g_wizard.editing && g_wizard.current_field == 0);
    row++;

    wizard_draw_field(row++, 4, "Station Name", g_wizard.station_name,
                      g_wizard.current_field == 1, g_wizard.editing && g_wizard.current_field == 1);
    row++;

    attron(COLOR_PAIR(3));
    mvprintw(row++, 4, "Note: Station name must EXACTLY match PLC configuration!");
    attroff(COLOR_PAIR(3));

    row += 2;
    mvprintw(row, 4, "Use UP/DOWN to navigate, ENTER to edit");
}

static void draw_step_sensors(void) {
    int row = 6;
    int max_y = getmaxy(stdscr);
    int max_display = max_y - 16;  /* Leave room for header/footer */

    mvprintw(row++, 4, "Sensor Hardware Discovery");
    row++;

    if (g_wizard.hw_scanning) {
        attron(COLOR_PAIR(3));  /* Yellow */
        mvprintw(row++, 4, "Scanning hardware buses... Please wait.");
        attroff(COLOR_PAIR(3));
        return;
    }

    if (!g_wizard.hw_scan_done) {
        mvprintw(row++, 4, "Press 'S' to scan for connected I2C and 1-Wire devices.");
        row++;
        mvprintw(row++, 4, "This will detect:");
        mvprintw(row++, 6, "- I2C devices: ADCs (ADS1115), sensors (BME280, SHT31)");
        mvprintw(row++, 6, "- 1-Wire devices: Temperature sensors (DS18B20)");
        row++;
        mvprintw(row++, 4, "Or press ENTER to skip and configure sensors manually later.");
    } else {
        /* Show discovery results */
        hw_discovery_result_t *hw = &g_wizard.hw_discovery;
        int total_devices = hw->i2c_count + hw->onewire_count;

        if (total_devices == 0) {
            attron(COLOR_PAIR(3));
            mvprintw(row++, 4, "No devices found.");
            attroff(COLOR_PAIR(3));
            row++;
            mvprintw(row++, 4, "Make sure devices are properly connected and 1-Wire/I2C enabled.");
            mvprintw(row++, 4, "You can configure sensors manually in the Sensors page (F2).");
        } else {
            attron(COLOR_PAIR(2));  /* Green */
            mvprintw(row++, 4, "Found %d device(s):", total_devices);
            attroff(COLOR_PAIR(2));
            row++;

            int display_row = 0;
            int skip_count = g_wizard.hw_scroll_offset;

            /* Display I2C devices */
            if (hw->i2c_count > 0) {
                if (skip_count == 0 && display_row < max_display) {
                    attron(A_BOLD);
                    mvprintw(row++, 4, "I2C Devices (%d):", hw->i2c_count);
                    attroff(A_BOLD);
                    display_row++;
                } else if (skip_count > 0) {
                    skip_count--;
                }

                for (int i = 0; i < hw->i2c_count && display_row < max_display; i++) {
                    if (skip_count > 0) {
                        skip_count--;
                        continue;
                    }
                    i2c_device_t *dev = &hw->i2c_devices[i];
                    mvprintw(row++, 6, "[I2C%d:0x%02X] %-16s - %s",
                             dev->bus, dev->address, dev->name, dev->description);
                    display_row++;
                }
            }

            /* Display 1-Wire devices */
            if (hw->onewire_count > 0 && display_row < max_display) {
                if (skip_count == 0 && display_row < max_display) {
                    row++;
                    attron(A_BOLD);
                    mvprintw(row++, 4, "1-Wire Devices (%d):", hw->onewire_count);
                    attroff(A_BOLD);
                    display_row += 2;
                } else if (skip_count > 0) {
                    skip_count--;
                }

                for (int i = 0; i < hw->onewire_count && display_row < max_display; i++) {
                    if (skip_count > 0) {
                        skip_count--;
                        continue;
                    }
                    onewire_device_t *dev = &hw->onewire_devices[i];
                    if (dev->last_value > -273.15f) {
                        mvprintw(row++, 6, "[1W] %-18s %-10s - %.1f C",
                                 dev->id, dev->name, dev->last_value);
                    } else {
                        mvprintw(row++, 6, "[1W] %-18s %-10s - %s",
                                 dev->id, dev->name, dev->description);
                    }
                    display_row++;
                }
            }

            /* Scroll indicator */
            int total_rows = hw->i2c_count + hw->onewire_count + 3;
            if (total_rows > max_display) {
                mvprintw(row + 1, 4, "[Use UP/DOWN to scroll, %d more]",
                         total_rows - max_display - g_wizard.hw_scroll_offset);
            }
        }

        row = max_y - 6;
        mvprintw(row++, 4, "Press 'S' to re-scan, ENTER to continue to next step.");
        mvprintw(row++, 4, "Detected devices will be available for sensor configuration.");
    }
}

static void draw_step_actuators(void) {
    int row = 6;
    mvprintw(row++, 4, "Actuator Configuration");
    row++;
    mvprintw(row++, 4, "Actuator setup can be completed after the wizard.");
    mvprintw(row++, 4, "Use the System page (F1) to configure actuators.");
    row++;

    mvprintw(row++, 4, "Supported actuator types:");
    mvprintw(row++, 6, "- Relay outputs (pumps, valves)");
    mvprintw(row++, 6, "- PWM outputs (variable speed control)");
    mvprintw(row++, 6, "- Pulse outputs (dosing pumps)");

    row++;
    mvprintw(row++, 4, "Safety features:");
    mvprintw(row++, 6, "- Configurable safe state (ON/OFF/HOLD)");
    mvprintw(row++, 6, "- Maximum on-time limits");
    mvprintw(row++, 6, "- Interlock groups");

    row += 2;
    mvprintw(row, 4, "Press ENTER to continue");
}

static void draw_step_confirm(void) {
    int row = 6;
    mvprintw(row++, 4, "Configuration Summary");
    row++;

    mvprintw(row++, 4, "Device Name:     %s", g_wizard.device_name);
    mvprintw(row++, 4, "Station Name:    %s", g_wizard.station_name);
    mvprintw(row++, 4, "Network:         %s (%s)",
             g_wizard.network_interface,
             g_wizard.use_dhcp ? "DHCP" : g_wizard.ip_address);
    if (g_wizard.board_detected) {
        mvprintw(row++, 4, "Board:           %s", g_wizard.board_name);
    }

    row += 2;
    attron(A_BOLD);
    mvprintw(row++, 4, "Press ENTER to save configuration");
    mvprintw(row++, 4, "Press ESC to go back and make changes");
    attroff(A_BOLD);
}

static void draw_step_complete(void) {
    int row = 8;
    attron(COLOR_PAIR(2) | A_BOLD);
    mvprintw(row++, 4, "Setup Complete!");
    attroff(COLOR_PAIR(2) | A_BOLD);

    row++;
    mvprintw(row++, 4, "Configuration has been saved.");
    row++;
    mvprintw(row++, 4, "Next steps:");
    mvprintw(row++, 6, "1. Connect sensors and actuators");
    mvprintw(row++, 6, "2. Configure sensors in the Sensors page (F2)");
    mvprintw(row++, 6, "3. Set up alarms in the Alarms page (F6)");
    mvprintw(row++, 6, "4. Import GSD file into your PLC/controller");
    mvprintw(row++, 6, "5. Establish PROFINET connection");

    row += 2;
    mvprintw(row, 4, "Press any key to exit wizard and start normal operation");
}

/* ============================================================================
 * Public API
 * ========================================================================== */

bool wizard_should_show(void) {
    return config_is_first_run(&g_app_config);
}

void page_wizard_init(void) {
    memset(&g_wizard, 0, sizeof(g_wizard));

    /* Set defaults from current config */
    SAFE_STRNCPY(g_wizard.device_name, g_app_config.system.device_name, sizeof(g_wizard.device_name));
    SAFE_STRNCPY(g_wizard.station_name, g_app_config.profinet.station_name, sizeof(g_wizard.station_name));
    SAFE_STRNCPY(g_wizard.network_interface, g_app_config.network.interface, sizeof(g_wizard.network_interface));
    g_wizard.use_dhcp = g_app_config.network.dhcp_enabled;
    SAFE_STRNCPY(g_wizard.ip_address, g_app_config.network.ip_address, sizeof(g_wizard.ip_address));
    SAFE_STRNCPY(g_wizard.netmask, g_app_config.network.netmask, sizeof(g_wizard.netmask));
    SAFE_STRNCPY(g_wizard.gateway, g_app_config.network.gateway, sizeof(g_wizard.gateway));

    /* Set reasonable defaults if empty */
    if (strlen(g_wizard.netmask) == 0) {
        SAFE_STRNCPY(g_wizard.netmask, "255.255.255.0", sizeof(g_wizard.netmask));
    }

    /* Detect board */
    board_info_t board_info;
    if (board_detect(&board_info) == RESULT_OK && board_info.confidence > 50) {
        g_wizard.board_type = board_info.type;
        SAFE_STRNCPY(g_wizard.board_name, board_info.model, sizeof(g_wizard.board_name));
        g_wizard.board_detected = true;
    }

    g_wizard.initialized = true;
}

void page_wizard_draw(void) {
    clear();

    /* Title */
    attron(A_BOLD);
    mvprintw(1, 2, "Water Treatment RTU - Setup Wizard");
    attroff(A_BOLD);

    /* Progress */
    wizard_draw_progress(3);

    /* Step content */
    switch (g_wizard.current_step) {
        case WIZARD_STEP_WELCOME:     draw_step_welcome(); break;
        case WIZARD_STEP_BOARD_DETECT: draw_step_board_detect(); break;
        case WIZARD_STEP_NETWORK:     draw_step_network(); break;
        case WIZARD_STEP_PROFINET:    draw_step_profinet(); break;
        case WIZARD_STEP_SENSORS:     draw_step_sensors(); break;
        case WIZARD_STEP_ACTUATORS:   draw_step_actuators(); break;
        case WIZARD_STEP_CONFIRM:     draw_step_confirm(); break;
        case WIZARD_STEP_COMPLETE:    draw_step_complete(); break;
        default: break;
    }

    /* Navigation help */
    int max_y = getmaxy(stdscr);
    mvprintw(max_y - 2, 2, "ESC: Back/Cancel | ENTER: Continue | TAB: Next Field");

    refresh();
}

bool page_wizard_input(int ch) {
    /* Handle editing mode */
    if (g_wizard.editing) {
        size_t len = strlen(g_wizard.edit_buffer);
        if (ch == '\n' || ch == KEY_ENTER) {
            /* Save edited value */
            g_wizard.editing = false;
            switch (g_wizard.current_step) {
                case WIZARD_STEP_NETWORK:
                    if (g_wizard.current_field == 0) {
                        SAFE_STRNCPY(g_wizard.network_interface, g_wizard.edit_buffer, sizeof(g_wizard.network_interface));
                    } else if (g_wizard.current_field == 2) {
                        SAFE_STRNCPY(g_wizard.ip_address, g_wizard.edit_buffer, sizeof(g_wizard.ip_address));
                    } else if (g_wizard.current_field == 3) {
                        SAFE_STRNCPY(g_wizard.netmask, g_wizard.edit_buffer, sizeof(g_wizard.netmask));
                    } else if (g_wizard.current_field == 4) {
                        SAFE_STRNCPY(g_wizard.gateway, g_wizard.edit_buffer, sizeof(g_wizard.gateway));
                    }
                    break;
                case WIZARD_STEP_PROFINET:
                    if (g_wizard.current_field == 0) {
                        SAFE_STRNCPY(g_wizard.device_name, g_wizard.edit_buffer, sizeof(g_wizard.device_name));
                    } else if (g_wizard.current_field == 1) {
                        SAFE_STRNCPY(g_wizard.station_name, g_wizard.edit_buffer, sizeof(g_wizard.station_name));
                    }
                    break;
                default:
                    break;
            }
            return true;
        } else if (ch == 27) {  /* ESC */
            g_wizard.editing = false;
            return true;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (len > 0) {
                g_wizard.edit_buffer[len - 1] = '\0';
            }
            return true;
        } else if (ch >= 32 && ch < 127 && len < sizeof(g_wizard.edit_buffer) - 1) {
            g_wizard.edit_buffer[len] = ch;
            g_wizard.edit_buffer[len + 1] = '\0';
            return true;
        }
        return true;
    }

    /* Normal navigation */
    switch (ch) {
        case 27:  /* ESC */
            if (g_wizard.current_step > WIZARD_STEP_WELCOME) {
                g_wizard.current_step--;
                g_wizard.current_field = 0;
            } else {
                g_wizard.cancelled = true;
            }
            return true;

        case '\n':
        case KEY_ENTER:
            if (g_wizard.current_step == WIZARD_STEP_CONFIRM) {
                /* Save configuration */
                SAFE_STRNCPY(g_app_config.system.device_name, g_wizard.device_name, sizeof(g_app_config.system.device_name));
                SAFE_STRNCPY(g_app_config.profinet.station_name, g_wizard.station_name, sizeof(g_app_config.profinet.station_name));
                SAFE_STRNCPY(g_app_config.network.interface, g_wizard.network_interface, sizeof(g_app_config.network.interface));
                g_app_config.network.dhcp_enabled = g_wizard.use_dhcp;
                if (!g_wizard.use_dhcp) {
                    SAFE_STRNCPY(g_app_config.network.ip_address, g_wizard.ip_address, sizeof(g_app_config.network.ip_address));
                    SAFE_STRNCPY(g_app_config.network.netmask, g_wizard.netmask, sizeof(g_app_config.network.netmask));
                    SAFE_STRNCPY(g_app_config.network.gateway, g_wizard.gateway, sizeof(g_app_config.network.gateway));
                }

                /* Save to config manager */
                config_set_string(&g_config_mgr, "system", "device_name", g_wizard.device_name);
                config_set_string(&g_config_mgr, "profinet", "station_name", g_wizard.station_name);
                config_set_string(&g_config_mgr, "network", "interface", g_wizard.network_interface);
                config_set_string(&g_config_mgr, "network", "dhcp_enabled", g_wizard.use_dhcp ? "true" : "false");

                /* Save to file */
                config_save_file(&g_config_mgr, "/etc/profinet-monitor/profinet-monitor.conf");

                g_wizard.current_step = WIZARD_STEP_COMPLETE;
            } else if (g_wizard.current_step == WIZARD_STEP_COMPLETE) {
                g_wizard.completed = true;
            } else if (g_wizard.current_step == WIZARD_STEP_NETWORK ||
                       g_wizard.current_step == WIZARD_STEP_PROFINET) {
                /* Start editing current field */
                g_wizard.editing = true;
                const char *current_value = "";
                if (g_wizard.current_step == WIZARD_STEP_NETWORK) {
                    switch (g_wizard.current_field) {
                        case 0: current_value = g_wizard.network_interface; break;
                        case 2: current_value = g_wizard.ip_address; break;
                        case 3: current_value = g_wizard.netmask; break;
                        case 4: current_value = g_wizard.gateway; break;
                    }
                } else if (g_wizard.current_step == WIZARD_STEP_PROFINET) {
                    switch (g_wizard.current_field) {
                        case 0: current_value = g_wizard.device_name; break;
                        case 1: current_value = g_wizard.station_name; break;
                    }
                }
                SAFE_STRNCPY(g_wizard.edit_buffer, current_value, sizeof(g_wizard.edit_buffer));
            } else {
                /* Move to next step */
                if (g_wizard.current_step < WIZARD_STEP_COMPLETE) {
                    g_wizard.current_step++;
                    g_wizard.current_field = 0;
                }
            }
            return true;

        case KEY_UP:
            if (g_wizard.current_field > 0) {
                g_wizard.current_field--;
            }
            return true;

        case KEY_DOWN:
        case '\t':
            g_wizard.current_field++;
            /* Limit based on step */
            if (g_wizard.current_step == WIZARD_STEP_NETWORK) {
                int max_field = g_wizard.use_dhcp ? 1 : 4;
                if (g_wizard.current_field > max_field) {
                    g_wizard.current_field = 0;
                }
            } else if (g_wizard.current_step == WIZARD_STEP_PROFINET) {
                if (g_wizard.current_field > 1) {
                    g_wizard.current_field = 0;
                }
            }
            return true;

        case ' ':
            /* Toggle checkbox */
            if (g_wizard.current_step == WIZARD_STEP_NETWORK && g_wizard.current_field == 1) {
                g_wizard.use_dhcp = !g_wizard.use_dhcp;
            }
            return true;

        case 'r':
        case 'R':
            if (g_wizard.current_step == WIZARD_STEP_BOARD_DETECT) {
                /* Re-detect board */
                board_info_t board_info;
                if (board_detect(&board_info) == RESULT_OK && board_info.confidence > 50) {
                    g_wizard.board_type = board_info.type;
                    SAFE_STRNCPY(g_wizard.board_name, board_info.model, sizeof(g_wizard.board_name));
                    g_wizard.board_detected = true;
                }
            }
            return true;

        case 's':
        case 'S':
            if (g_wizard.current_step == WIZARD_STEP_SENSORS) {
                /* Trigger hardware scan */
                g_wizard.hw_scanning = true;
                page_wizard_draw();  /* Show scanning message */
                refresh();

                /* Perform the scan */
                memset(&g_wizard.hw_discovery, 0, sizeof(g_wizard.hw_discovery));
                hw_discover_all(&g_wizard.hw_discovery);

                g_wizard.hw_scanning = false;
                g_wizard.hw_scan_done = true;
                g_wizard.hw_scroll_offset = 0;
            }
            return true;
    }

    /* Handle scrolling in sensors step */
    if (g_wizard.current_step == WIZARD_STEP_SENSORS && g_wizard.hw_scan_done) {
        int total_devices = g_wizard.hw_discovery.i2c_count +
                           g_wizard.hw_discovery.onewire_count;
        if (ch == KEY_UP && g_wizard.hw_scroll_offset > 0) {
            g_wizard.hw_scroll_offset--;
            return true;
        } else if (ch == KEY_DOWN && g_wizard.hw_scroll_offset < total_devices) {
            g_wizard.hw_scroll_offset++;
            return true;
        }
    }

    return false;
}

void page_wizard_cleanup(void) {
    g_wizard.initialized = false;
}

result_t wizard_run(void) {
    if (!g_wizard.initialized) {
        page_wizard_init();
    }

    while (!g_wizard.completed && !g_wizard.cancelled) {
        page_wizard_draw();
        int ch = getch();
        page_wizard_input(ch);
    }

    page_wizard_cleanup();

    return g_wizard.completed ? RESULT_OK : RESULT_ERROR;
}
