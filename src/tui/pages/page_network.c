/**
 * @file page_network.c
 * @brief Network configuration page
 */

#include "page_network.h"
#include "../tui_common.h"
#include "config/config.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ifaddrs.h>

#define MAX_INTERFACES 8
#define MAX_FIELDS 12

typedef struct {
    char name[32];
    char ip_addr[16];
    char netmask[16];
    char mac_addr[18];
    bool is_up;
    bool has_link;
} interface_info_t;

typedef struct {
    const char *label;
    char value[64];
    bool editable;
    const char *config_key;
} field_t;

static struct {
    WINDOW *win;
    
    interface_info_t interfaces[MAX_INTERFACES];
    int interface_count;
    int selected_interface;
    
    field_t fields[MAX_FIELDS];
    int field_count;
    int selected_field;
    
    bool editing;
    char edit_buffer[64];
    int edit_pos;
    
    int view_mode;  // 0 = interfaces, 1 = config
} g_page = {0};

static void get_interface_info(void) {
    g_page.interface_count = 0;
    
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return;
    
    for (ifa = ifaddr; ifa != NULL && g_page.interface_count < MAX_INTERFACES; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;
        
        // Check if we already have this interface
        bool found = false;
        for (int i = 0; i < g_page.interface_count; i++) {
            if (strcmp(g_page.interfaces[i].name, ifa->ifa_name) == 0) {
                found = true;
                break;
            }
        }
        if (found) continue;
        
        interface_info_t *iface = &g_page.interfaces[g_page.interface_count];
        SAFE_STRNCPY(iface->name, ifa->ifa_name, sizeof(iface->name));
        
        // IP address
        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
        inet_ntop(AF_INET, &addr->sin_addr, iface->ip_addr, sizeof(iface->ip_addr));
        
        // Netmask
        struct sockaddr_in *netmask = (struct sockaddr_in *)ifa->ifa_netmask;
        if (netmask) {
            inet_ntop(AF_INET, &netmask->sin_addr, iface->netmask, sizeof(iface->netmask));
        }
        
        // Flags
        iface->is_up = (ifa->ifa_flags & IFF_UP) != 0;
        iface->has_link = (ifa->ifa_flags & IFF_RUNNING) != 0;
        
        // MAC address
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr;
            strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
            if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                unsigned char *mac = (unsigned char *)ifr.ifr_hwaddr.sa_data;
                snprintf(iface->mac_addr, sizeof(iface->mac_addr),
                        "%02X:%02X:%02X:%02X:%02X:%02X",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            }
            close(sock);
        }
        
        g_page.interface_count++;
    }
    
    freeifaddrs(ifaddr);
}

static void load_network_config(void) {
    g_page.field_count = 0;
    
    app_config_t *cfg = tui_get_app_config();
    if (!cfg) return;
    
    field_t *f;
    
    // Interface
    f = &g_page.fields[g_page.field_count++];
    f->label = "Interface";
    f->editable = true;
    f->config_key = "interface";
    SAFE_STRNCPY(f->value, cfg->network.interface, sizeof(f->value));
    
    // DHCP
    f = &g_page.fields[g_page.field_count++];
    f->label = "DHCP Enabled";
    f->editable = true;
    f->config_key = "dhcp_enabled";
    strcpy(f->value, cfg->network.dhcp_enabled ? "true" : "false");
    
    // IP Address
    f = &g_page.fields[g_page.field_count++];
    f->label = "IP Address";
    f->editable = true;
    f->config_key = "ip_address";
    SAFE_STRNCPY(f->value, cfg->network.ip_address, sizeof(f->value));
    
    // Netmask
    f = &g_page.fields[g_page.field_count++];
    f->label = "Netmask";
    f->editable = true;
    f->config_key = "netmask";
    SAFE_STRNCPY(f->value, cfg->network.netmask, sizeof(f->value));
    
    // Gateway
    f = &g_page.fields[g_page.field_count++];
    f->label = "Gateway";
    f->editable = true;
    f->config_key = "gateway";
    SAFE_STRNCPY(f->value, cfg->network.gateway, sizeof(f->value));
    
    // PROFINET Station Name
    f = &g_page.fields[g_page.field_count++];
    f->label = "Station Name";
    f->editable = true;
    f->config_key = "station_name";
    SAFE_STRNCPY(f->value, cfg->profinet.station_name, sizeof(f->value));
    
    // Vendor ID
    f = &g_page.fields[g_page.field_count++];
    f->label = "Vendor ID";
    f->editable = true;
    f->config_key = "vendor_id";
    snprintf(f->value, sizeof(f->value), "0x%04X", cfg->profinet.vendor_id);
    
    // Device ID
    f = &g_page.fields[g_page.field_count++];
    f->label = "Device ID";
    f->editable = true;
    f->config_key = "device_id";
    snprintf(f->value, sizeof(f->value), "0x%04X", cfg->profinet.device_id);
}

static void save_field(int idx) {
    if (idx < 0 || idx >= g_page.field_count) return;
    
    field_t *f = &g_page.fields[idx];
    if (!f->editable) return;
    
    config_manager_t *cfg_mgr = tui_get_config_manager();
    app_config_t *app_cfg = tui_get_app_config();
    if (!cfg_mgr || !app_cfg) return;
    
    // Update config based on key
    if (strcmp(f->config_key, "interface") == 0) {
        SAFE_STRNCPY(app_cfg->network.interface, f->value, sizeof(app_cfg->network.interface));
        config_set_string(cfg_mgr, "network", f->config_key, f->value);
    } else if (strcmp(f->config_key, "dhcp_enabled") == 0) {
        app_cfg->network.dhcp_enabled = (strcmp(f->value, "true") == 0);
        config_set_string(cfg_mgr, "network", f->config_key, f->value);
    } else if (strcmp(f->config_key, "ip_address") == 0) {
        SAFE_STRNCPY(app_cfg->network.ip_address, f->value, sizeof(app_cfg->network.ip_address));
        config_set_string(cfg_mgr, "network", f->config_key, f->value);
    } else if (strcmp(f->config_key, "netmask") == 0) {
        SAFE_STRNCPY(app_cfg->network.netmask, f->value, sizeof(app_cfg->network.netmask));
        config_set_string(cfg_mgr, "network", f->config_key, f->value);
    } else if (strcmp(f->config_key, "gateway") == 0) {
        SAFE_STRNCPY(app_cfg->network.gateway, f->value, sizeof(app_cfg->network.gateway));
        config_set_string(cfg_mgr, "network", f->config_key, f->value);
    } else if (strcmp(f->config_key, "station_name") == 0) {
        SAFE_STRNCPY(app_cfg->profinet.station_name, f->value, sizeof(app_cfg->profinet.station_name));
        config_set_string(cfg_mgr, "profinet", f->config_key, f->value);
    } else if (strcmp(f->config_key, "vendor_id") == 0) {
        app_cfg->profinet.vendor_id = (uint16_t)strtol(f->value, NULL, 0);
        config_set_string(cfg_mgr, "profinet", f->config_key, f->value);
    } else if (strcmp(f->config_key, "device_id") == 0) {
        app_cfg->profinet.device_id = (uint16_t)strtol(f->value, NULL, 0);
        config_set_string(cfg_mgr, "profinet", f->config_key, f->value);
    }
    
    tui_set_status("Saved: %s", f->label);
}

static void draw_interfaces(WINDOW *win) {
    int row = 3;
    
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, row++, 2, "Network Interfaces");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    row++;
    
    wattron(win, A_BOLD);
    mvwprintw(win, row++, 4, "%-12s %-16s %-16s %-18s %-6s",
              "Interface", "IP Address", "Netmask", "MAC Address", "Status");
    wattroff(win, A_BOLD);
    
    mvwhline(win, row++, 4, ACS_HLINE, 70);
    
    for (int i = 0; i < g_page.interface_count; i++) {
        interface_info_t *iface = &g_page.interfaces[i];
        
        if (i == g_page.selected_interface && g_page.view_mode == 0) {
            wattron(win, A_REVERSE);
        }
        
        int color = iface->has_link ? TUI_COLOR_STATUS : TUI_COLOR_ERROR;
        const char *status = iface->has_link ? "UP" : "DOWN";
        
        mvwprintw(win, row, 4, "%-12s %-16s %-16s %-18s ",
                  iface->name, iface->ip_addr, iface->netmask, iface->mac_addr);
        
        wattron(win, COLOR_PAIR(color));
        wprintw(win, "%-6s", status);
        wattroff(win, COLOR_PAIR(color));
        
        if (i == g_page.selected_interface && g_page.view_mode == 0) {
            wattroff(win, A_REVERSE);
        }
        
        row++;
    }
    
    if (g_page.interface_count == 0) {
        wattron(win, COLOR_PAIR(TUI_COLOR_WARNING));
        mvwprintw(win, row, 4, "No network interfaces found");
        wattroff(win, COLOR_PAIR(TUI_COLOR_WARNING));
    }
}

static void draw_config(WINDOW *win) {
    int row = 3 + g_page.interface_count + 4;
    int max_x = getmaxx(win);
    
    wattron(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwprintw(win, row++, 2, "Network Configuration");
    wattroff(win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    row++;
    
    for (int i = 0; i < g_page.field_count; i++) {
        field_t *f = &g_page.fields[i];
        
        if (i == g_page.selected_field && g_page.view_mode == 1) {
            wattron(win, A_REVERSE);
        }
        
        mvwprintw(win, row, 4, "%-14s: ", f->label);
        
        if (g_page.editing && i == g_page.selected_field) {
            wattron(win, COLOR_PAIR(TUI_COLOR_INPUT));
            wprintw(win, "%-30s", g_page.edit_buffer);
            wattroff(win, COLOR_PAIR(TUI_COLOR_INPUT));
        } else {
            if (f->editable) {
                wattron(win, COLOR_PAIR(TUI_COLOR_STATUS));
            }
            wprintw(win, "%-30s", f->value);
            if (f->editable) {
                wattroff(win, COLOR_PAIR(TUI_COLOR_STATUS));
            }
        }
        
        if (f->editable && g_page.view_mode == 1 && !g_page.editing) {
            mvwprintw(win, row, max_x - 10, "[Edit]");
        }
        
        if (i == g_page.selected_field && g_page.view_mode == 1) {
            wattroff(win, A_REVERSE);
        }
        
        row++;
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

void page_network_init(WINDOW *win) {
    g_page.win = win;
    g_page.view_mode = 0;
    g_page.selected_interface = 0;
    g_page.selected_field = 0;
    g_page.editing = false;
    get_interface_info();
    load_network_config();
}

void page_network_draw(WINDOW *win) {
    draw_interfaces(win);
    draw_config(win);
    draw_help(win);
}

void page_network_input(WINDOW *win, int ch) {
    UNUSED(win);
    
    if (g_page.editing) {
        switch (ch) {
            case 27:  // Escape
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
                if (g_page.selected_interface > 0) g_page.selected_interface--;
            } else {
                if (g_page.selected_field > 0) g_page.selected_field--;
            }
            break;
            
        case KEY_DOWN:
            if (g_page.view_mode == 0) {
                if (g_page.selected_interface < g_page.interface_count - 1) 
                    g_page.selected_interface++;
            } else {
                if (g_page.selected_field < g_page.field_count - 1) 
                    g_page.selected_field++;
            }
            break;
            
        case '\n':
        case KEY_ENTER:
            if (g_page.view_mode == 1 && g_page.fields[g_page.selected_field].editable) {
                g_page.editing = true;
                SAFE_STRNCPY(g_page.edit_buffer,
                            g_page.fields[g_page.selected_field].value,
                            sizeof(g_page.edit_buffer));
                g_page.edit_pos = strlen(g_page.edit_buffer);
            }
            break;
            
        case 'r':
        case 'R':
            get_interface_info();
            load_network_config();
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

void page_network_cleanup(void) {
    g_page.win = NULL;
}
