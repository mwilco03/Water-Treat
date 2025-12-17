/**
 * @file tui_main.c
 * @brief Main TUI loop and page management
 */

#include "tui_main.h"
#include "tui_common.h"
#include "pages/page_system.h"
#include "pages/page_sensors.h"
#include "pages/page_network.h"
#include "pages/page_profinet.h"
#include "pages/page_status.h"
#include "pages/page_alarms.h"
#include "pages/page_logging.h"
#include "pages/page_actuators.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <signal.h>
#include <string.h>

#define TUI_REFRESH_MS      100
#define STATUS_BAR_HEIGHT   1
#define FOOTER_HEIGHT       1

typedef enum {
    PAGE_SYSTEM = 0,
    PAGE_SENSORS,
    PAGE_NETWORK,
    PAGE_PROFINET,
    PAGE_STATUS,
    PAGE_ALARMS,
    PAGE_LOGGING,
    PAGE_ACTUATORS,
    PAGE_COUNT
} tui_page_t;

typedef struct {
    const char *title;
    int key;
    void (*init)(WINDOW *win);
    void (*draw)(WINDOW *win);
    void (*handle_input)(WINDOW *win, int ch);
    void (*cleanup)(void);
} page_def_t;

static struct {
    WINDOW *main_win;
    WINDOW *status_bar;
    WINDOW *footer;
    
    tui_page_t current_page;
    bool running;
    bool needs_redraw;
    
    database_t *db;
    config_manager_t *config;
    app_config_t *app_config;
    
    char status_message[256];
    time_t status_time;
} g_tui = {0};

static page_def_t pages[PAGE_COUNT] = {
    {"System",    KEY_F(1), page_system_init,    page_system_draw,    page_system_input,    page_system_cleanup},
    {"Sensors",   KEY_F(2), page_sensors_init,   page_sensors_draw,   page_sensors_input,   page_sensors_cleanup},
    {"Network",   KEY_F(3), page_network_init,   page_network_draw,   page_network_input,   page_network_cleanup},
    {"PROFINET",  KEY_F(4), page_profinet_init,  page_profinet_draw,  page_profinet_input,  page_profinet_cleanup},
    {"Status",    KEY_F(5), page_status_init,    page_status_draw,    page_status_input,    page_status_cleanup},
    {"Alarms",    KEY_F(6), page_alarms_init,    page_alarms_draw,    page_alarms_input,    page_alarms_cleanup},
    {"Logging",   KEY_F(7), page_logging_init,   page_logging_draw,   page_logging_input,   page_logging_cleanup},
    {"Actuators", KEY_F(8), page_actuators_init, page_actuators_draw, page_actuators_input, page_actuators_cleanup},
};

/* ============================================================================
 * Internal Functions
 * ========================================================================== */

static void draw_status_bar(void) {
    int max_x = getmaxx(g_tui.status_bar);
    
    wattron(g_tui.status_bar, A_BOLD | COLOR_PAIR(TUI_COLOR_HEADER));
    mvwhline(g_tui.status_bar, 0, 0, ' ', max_x);
    
    mvwprintw(g_tui.status_bar, 0, 2, "PROFINET Monitor v%s", VERSION_STRING);
    
    if (g_tui.app_config) {
        mvwprintw(g_tui.status_bar, 0, 30, "| %s", g_tui.app_config->system.device_name);
    }
    
    // Current time
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
    mvwprintw(g_tui.status_bar, 0, max_x - 12, "%s", time_str);
    
    wattroff(g_tui.status_bar, A_BOLD | COLOR_PAIR(TUI_COLOR_HEADER));
    wrefresh(g_tui.status_bar);
}

static void draw_footer(void) {
    int max_x = getmaxx(g_tui.footer);
    
    wattron(g_tui.footer, COLOR_PAIR(TUI_COLOR_HEADER));
    mvwhline(g_tui.footer, 0, 0, ' ', max_x);
    
    int x = 2;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == g_tui.current_page) {
            wattron(g_tui.footer, A_REVERSE);
        }
        mvwprintw(g_tui.footer, 0, x, "F%d:%s", i + 1, pages[i].title);
        if (i == g_tui.current_page) {
            wattroff(g_tui.footer, A_REVERSE);
        }
        x += strlen(pages[i].title) + 5;
    }
    
    mvwprintw(g_tui.footer, 0, max_x - 10, "F10:Quit");
    
    wattroff(g_tui.footer, COLOR_PAIR(TUI_COLOR_HEADER));
    wrefresh(g_tui.footer);
}

static void draw_page_header(void) {
    int max_x = getmaxx(g_tui.main_win);
    
    wattron(g_tui.main_win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    mvwhline(g_tui.main_win, 0, 0, ' ', max_x);
    mvwprintw(g_tui.main_win, 0, 2, " %s ", pages[g_tui.current_page].title);
    wattroff(g_tui.main_win, A_BOLD | COLOR_PAIR(TUI_COLOR_TITLE));
    
    // Status message
    if (g_tui.status_message[0] && (time(NULL) - g_tui.status_time) < 5) {
        wattron(g_tui.main_win, COLOR_PAIR(TUI_COLOR_STATUS));
        mvwprintw(g_tui.main_win, 0, max_x - strlen(g_tui.status_message) - 4, 
                  " %s ", g_tui.status_message);
        wattroff(g_tui.main_win, COLOR_PAIR(TUI_COLOR_STATUS));
    }
}

static void switch_page(tui_page_t new_page) {
    if (new_page == g_tui.current_page || new_page >= PAGE_COUNT) return;
    
    // Cleanup current page
    if (pages[g_tui.current_page].cleanup) {
        pages[g_tui.current_page].cleanup();
    }
    
    g_tui.current_page = new_page;
    
    // Initialize new page
    if (pages[g_tui.current_page].init) {
        pages[g_tui.current_page].init(g_tui.main_win);
    }
    
    g_tui.needs_redraw = true;
}

static void handle_resize(void) {
    endwin();
    refresh();
    
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    wresize(g_tui.status_bar, STATUS_BAR_HEIGHT, max_x);
    mvwin(g_tui.status_bar, 0, 0);
    
    wresize(g_tui.main_win, max_y - STATUS_BAR_HEIGHT - FOOTER_HEIGHT, max_x);
    mvwin(g_tui.main_win, STATUS_BAR_HEIGHT, 0);
    
    wresize(g_tui.footer, FOOTER_HEIGHT, max_x);
    mvwin(g_tui.footer, max_y - FOOTER_HEIGHT, 0);
    
    g_tui.needs_redraw = true;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

result_t tui_init(database_t *db, config_manager_t *config, app_config_t *app_config) {
    g_tui.db = db;
    g_tui.config = config;
    g_tui.app_config = app_config;
    
    // Initialize ncurses
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(TUI_REFRESH_MS);
    
    // Initialize colors
    if (has_colors()) {
        start_color();
        use_default_colors();
        
        init_pair(TUI_COLOR_NORMAL, COLOR_WHITE, -1);
        init_pair(TUI_COLOR_HEADER, COLOR_WHITE, COLOR_BLUE);
        init_pair(TUI_COLOR_TITLE, COLOR_CYAN, -1);
        init_pair(TUI_COLOR_STATUS, COLOR_GREEN, -1);
        init_pair(TUI_COLOR_ERROR, COLOR_RED, -1);
        init_pair(TUI_COLOR_WARNING, COLOR_YELLOW, -1);
        init_pair(TUI_COLOR_HIGHLIGHT, COLOR_BLACK, COLOR_CYAN);
        init_pair(TUI_COLOR_INPUT, COLOR_WHITE, COLOR_BLUE);
    }
    
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    // Create windows
    g_tui.status_bar = newwin(STATUS_BAR_HEIGHT, max_x, 0, 0);
    g_tui.main_win = newwin(max_y - STATUS_BAR_HEIGHT - FOOTER_HEIGHT, max_x, STATUS_BAR_HEIGHT, 0);
    g_tui.footer = newwin(FOOTER_HEIGHT, max_x, max_y - FOOTER_HEIGHT, 0);
    
    keypad(g_tui.main_win, TRUE);
    
    // Set shared context for pages
    tui_set_context(db, config, app_config);
    
    // Initialize first page
    g_tui.current_page = PAGE_STATUS;
    if (pages[g_tui.current_page].init) {
        pages[g_tui.current_page].init(g_tui.main_win);
    }
    
    g_tui.needs_redraw = true;
    LOG_INFO("TUI initialized");
    
    return RESULT_OK;
}

void tui_run(void) {
    g_tui.running = true;
    
    while (g_tui.running) {
        // Draw status bar and footer
        draw_status_bar();
        draw_footer();
        
        // Draw current page
        if (g_tui.needs_redraw) {
            werase(g_tui.main_win);
            draw_page_header();
            
            if (pages[g_tui.current_page].draw) {
                pages[g_tui.current_page].draw(g_tui.main_win);
            }
            
            wrefresh(g_tui.main_win);
            g_tui.needs_redraw = false;
        }
        
        // Handle input
        int ch = wgetch(g_tui.main_win);
        
        if (ch == ERR) {
            // Timeout, refresh status
            g_tui.needs_redraw = true;
            continue;
        }
        
        // Global keys
        switch (ch) {
            case KEY_F(1): switch_page(PAGE_SYSTEM); break;
            case KEY_F(2): switch_page(PAGE_SENSORS); break;
            case KEY_F(3): switch_page(PAGE_NETWORK); break;
            case KEY_F(4): switch_page(PAGE_PROFINET); break;
            case KEY_F(5): switch_page(PAGE_STATUS); break;
            case KEY_F(6): switch_page(PAGE_ALARMS); break;
            case KEY_F(7): switch_page(PAGE_LOGGING); break;
            case KEY_F(8): switch_page(PAGE_ACTUATORS); break;
            case KEY_F(10):
            case 'q':
            case 'Q':
                g_tui.running = false;
                break;
            case KEY_RESIZE:
                handle_resize();
                break;
            default:
                // Pass to current page
                if (pages[g_tui.current_page].handle_input) {
                    pages[g_tui.current_page].handle_input(g_tui.main_win, ch);
                    g_tui.needs_redraw = true;
                }
                break;
        }
    }
}

void tui_shutdown(void) {
    // Cleanup current page
    if (pages[g_tui.current_page].cleanup) {
        pages[g_tui.current_page].cleanup();
    }
    
    // Destroy windows
    if (g_tui.status_bar) delwin(g_tui.status_bar);
    if (g_tui.main_win) delwin(g_tui.main_win);
    if (g_tui.footer) delwin(g_tui.footer);
    
    endwin();
    LOG_INFO("TUI shutdown");
}

void tui_set_status(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_tui.status_message, sizeof(g_tui.status_message), fmt, args);
    va_end(args);
    g_tui.status_time = time(NULL);
    g_tui.needs_redraw = true;
}

void tui_request_redraw(void) {
    g_tui.needs_redraw = true;
}

void tui_quit(void) {
    g_tui.running = false;
}

bool tui_is_running(void) {
    return g_tui.running;
}
