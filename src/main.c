/**
 * PROFINET Monitor - Main Entry Point
 */

#include "common.h"
#include "utils/logger.h"
#include "config/config.h"
#include "db/database.h"
#include <signal.h>
#include <unistd.h>
#include <ncurses.h>

static volatile bool g_running = true;

static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

int main(int argc, char *argv[]) {
    printf("PROFINET Monitor v%s\n", VERSION_STRING);
    printf("Raspberry Pi Sensor Hub with PROFINET Interface\n\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize logger
    logger_config_t log_cfg = {
        .level = LOG_LEVEL_INFO,
        .destinations = LOG_DEST_CONSOLE,
        .include_timestamp = true
    };
    logger_init(&log_cfg);
    LOG_INFO("Starting PROFINET Monitor...");
    
    // Load configuration
    config_manager_t config_mgr;
    config_manager_init(&config_mgr);
    
    app_config_t app_config;
    config_get_defaults(&app_config);
    
    const char *config_path = "/etc/profinet-monitor/profinet-monitor.conf";
    if (access(config_path, R_OK) == 0) {
        config_load_file(&config_mgr, config_path);
        config_load_app_config(&config_mgr, &app_config);
    }
    
    // Initialize database
    database_t db;
    if (database_init(&db, app_config.database.path) != RESULT_OK) {
        LOG_ERROR("Failed to initialize database");
        return 1;
    }
    
    LOG_INFO("System initialized successfully");
    LOG_INFO("Device: %s", app_config.system.device_name);
    LOG_INFO("PROFINET Station: %s", app_config.profinet.station_name);
    
    // Initialize ncurses for TUI
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    
    init_pair(1, COLOR_WHITE, COLOR_BLUE);
    init_pair(2, COLOR_CYAN, COLOR_BLACK);
    init_pair(3, COLOR_GREEN, COLOR_BLACK);
    init_pair(4, COLOR_RED, COLOR_BLACK);
    init_pair(5, COLOR_YELLOW, COLOR_BLACK);
    
    timeout(100);
    
    int current_page = 5; // Start on status page
    
    while (g_running) {
        clear();
        
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        
        // Title bar
        attron(A_BOLD | COLOR_PAIR(1));
        mvhline(0, 0, ' ', max_x);
        mvprintw(0, 2, "PROFINET Monitor v%s - %s", VERSION_STRING, app_config.system.device_name);
        attroff(A_BOLD | COLOR_PAIR(1));
        
        // Page content
        attron(COLOR_PAIR(2));
        mvprintw(3, 2, "Current Page: ");
        attroff(COLOR_PAIR(2));
        
        switch (current_page) {
            case 1: mvprintw(3, 16, "System Configuration"); break;
            case 2: mvprintw(3, 16, "Sensor Management"); break;
            case 3: mvprintw(3, 16, "Network Settings"); break;
            case 4: mvprintw(3, 16, "Modbus Configuration"); break;
            case 5: mvprintw(3, 16, "Live Status"); break;
            case 6: mvprintw(3, 16, "Alarm Management"); break;
            case 7: mvprintw(3, 16, "Data Logging"); break;
        }
        
        attron(COLOR_PAIR(3));
        mvprintw(5, 2, "PROFINET: %s", app_config.profinet.enabled ? "Enabled" : "Disabled");
        mvprintw(6, 2, "Station Name: %s", app_config.profinet.station_name);
        mvprintw(7, 2, "Vendor ID: 0x%04X  Device ID: 0x%04X", 
                 app_config.profinet.vendor_id, app_config.profinet.device_id);
        mvprintw(9, 2, "Database: %s", database_is_connected(&db) ? "Connected" : "Disconnected");
        mvprintw(10, 2, "Logging: %s (interval: %ds)", 
                 app_config.logging.enabled ? "Enabled" : "Disabled",
                 app_config.logging.interval_seconds);
        attroff(COLOR_PAIR(3));
        
        mvprintw(12, 2, "Press F1-F7 to switch pages, F10 or 'q' to quit");
        
        // Footer
        attron(COLOR_PAIR(1));
        mvhline(max_y - 1, 0, ' ', max_x);
        mvprintw(max_y - 1, 2, 
                "F1:System F2:Sensors F3:Network F4:Modbus F5:Status F6:Alarms F7:Logging F10:Quit");
        attroff(COLOR_PAIR(1));
        
        refresh();
        
        int ch = getch();
        switch (ch) {
            case KEY_F(1): current_page = 1; break;
            case KEY_F(2): current_page = 2; break;
            case KEY_F(3): current_page = 3; break;
            case KEY_F(4): current_page = 4; break;
            case KEY_F(5): current_page = 5; break;
            case KEY_F(6): current_page = 6; break;
            case KEY_F(7): current_page = 7; break;
            case KEY_F(10):
            case 'q':
            case 'Q':
                g_running = false;
                break;
        }
    }
    
    endwin();
    
    LOG_INFO("Shutting down...");
    database_close(&db);
    config_manager_destroy(&config_mgr);
    logger_shutdown();
    
    printf("PROFINET Monitor stopped.\n");
    return 0;
}
