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
#include "pages/page_login.h"
#include "auth/auth.h"
#include "actuators/actuator_manager.h"  /* Global E-STOP per DEVELOPMENT_GUIDELINES.md */
#include "utils/logger.h"
#include <ncurses.h>
#include <signal.h>
#include <string.h>

/* External actuator manager for global E-STOP */
extern actuator_manager_t g_actuator_mgr;

#define TUI_REFRESH_MS      100
#define STATUS_BAR_HEIGHT   1
#define FOOTER_HEIGHT       1
#define MAX_SCREEN_HISTORY  16
#define TUI_MSG_RING_SIZE   32
#define TUI_MSG_MAX_LEN     256

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

/* Message ring buffer entry for TUI log messages */
typedef struct {
    char message[TUI_MSG_MAX_LEN];
    int level;
    time_t timestamp;
} tui_msg_entry_t;

static struct {
    WINDOW *main_win;
    WINDOW *status_bar;
    WINDOW *footer;

    tui_page_t current_page;
    bool running;
    bool initialized;  /* True after tui_init(), false after tui_shutdown() */
    bool needs_redraw;

    database_t *db;
    config_manager_t *config;
    app_config_t *app_config;

    char status_message[256];
    time_t status_time;

    /* Screen history for ESC navigation */
    tui_page_t history_stack[MAX_SCREEN_HISTORY];
    int history_top;

    /* Message ring buffer for log messages when TUI is active */
    tui_msg_entry_t msg_ring[TUI_MSG_RING_SIZE];
    int msg_ring_head;
    int msg_ring_count;
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
    
    mvwprintw(g_tui.status_bar, 0, 2, "Water-Treat RTU v%s", VERSION_STRING);
    
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
        if ((tui_page_t)i == g_tui.current_page) {
            wattron(g_tui.footer, A_REVERSE);
        }
        mvwprintw(g_tui.footer, 0, x, "F%d:%s", i + 1, pages[i].title);
        if ((tui_page_t)i == g_tui.current_page) {
            wattroff(g_tui.footer, A_REVERSE);
        }
        x += strlen(pages[i].title) + 5;
    }

    /* Show navigation hints on the right side */
    mvwprintw(g_tui.footer, 0, max_x - 45, "<->:Cycle");  /* Arrow key cycling */
    wattron(g_tui.footer, A_BOLD | COLOR_PAIR(TUI_COLOR_ERROR));
    mvwprintw(g_tui.footer, 0, max_x - 34, "E:ESTOP");    /* Global E-STOP per guidelines */
    wattroff(g_tui.footer, A_BOLD | COLOR_PAIR(TUI_COLOR_ERROR));
    if (g_tui.history_top > 0) {
        mvwprintw(g_tui.footer, 0, max_x - 24, "ESC:Back");
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

/* Push current page onto history stack before switching */
static void history_push(tui_page_t page) {
    if (g_tui.history_top < MAX_SCREEN_HISTORY - 1) {
        g_tui.history_stack[g_tui.history_top++] = page;
    }
}

/* Pop previous page from history stack */
static tui_page_t history_pop(void) {
    if (g_tui.history_top > 0) {
        return g_tui.history_stack[--g_tui.history_top];
    }
    return PAGE_COUNT;  /* Invalid - signals we're at root */
}

static void switch_page(tui_page_t new_page) {
    if (new_page == g_tui.current_page || new_page >= PAGE_COUNT) return;

    /* Push current page onto history before switching */
    history_push(g_tui.current_page);

    /* Cleanup current page */
    if (pages[g_tui.current_page].cleanup) {
        pages[g_tui.current_page].cleanup();
    }

    g_tui.current_page = new_page;

    /* Initialize new page */
    if (pages[g_tui.current_page].init) {
        pages[g_tui.current_page].init(g_tui.main_win);
    }

    g_tui.needs_redraw = true;
}

/* Navigate back to previous screen (for ESC key) */
static bool navigate_back(void) {
    tui_page_t prev = history_pop();
    if (prev < PAGE_COUNT) {
        /* Cleanup current page */
        if (pages[g_tui.current_page].cleanup) {
            pages[g_tui.current_page].cleanup();
        }

        g_tui.current_page = prev;

        /* Initialize previous page */
        if (pages[g_tui.current_page].init) {
            pages[g_tui.current_page].init(g_tui.main_win);
        }

        g_tui.needs_redraw = true;
        return true;  /* Successfully navigated back */
    }
    return false;  /* At root, no previous screen */
}

/**
 * Switch to a page WITHOUT modifying navigation history.
 * Used for Left/Right arrow cycling where user is "browsing"
 * rather than intentionally navigating to a destination.
 *
 * @param page Target page to display
 */
static void cycle_page(tui_page_t page) {
    if (page == g_tui.current_page) {
        return;  /* Already there */
    }

    if (page < 0 || page >= PAGE_COUNT) {
        LOG_WARNING("cycle_page: invalid page %d", page);
        return;
    }

    /* Clean up current page */
    if (pages[g_tui.current_page].cleanup) {
        pages[g_tui.current_page].cleanup();
    }

    /* Switch without history modification */
    g_tui.current_page = page;

    /* Initialize new page */
    if (pages[g_tui.current_page].init) {
        pages[g_tui.current_page].init(g_tui.main_win);
    }

    /* Force redraw */
    g_tui.needs_redraw = true;

    LOG_DEBUG("Cycled to page %d (%s)", page, pages[page].title);
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
    keypad(g_tui.main_win, TRUE); // Enable keypad input for main window
    wtimeout(g_tui.main_win, TUI_REFRESH_MS); // Set timeout for main window

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
    g_tui.initialized = true;
    LOG_INFO("TUI initialized");

    return RESULT_OK;
}

void tui_run(void) {
    /* Require login before accessing main interface
     * Block input during login to prevent pulsing redraw (especially over SSH).
     */
    wtimeout(stdscr, -1);
    result_t login_result = page_login_run();
    
    /* Restore periodic refresh for the main interface */
    wtimeout(stdscr, TUI_REFRESH_MS);
    wtimeout(g_tui.main_win, TUI_REFRESH_MS);
    
    if (login_result != RESULT_OK) {
        LOG_INFO("Login cancelled or failed - exiting TUI");
        return;
    }
    LOG_INFO("Login successful - starting main interface");
    
    /* Login draws on stdscr; ensure we wipe any remnants before the windowed UI starts */
    clearok(stdscr, TRUE);
    clear();
    refresh();
    g_tui.needs_redraw = true;

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
        
        /* Global keys */
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

            /*
             * GLOBAL E-STOP: 'E' key triggers emergency stop from ANY screen
             * Per DEVELOPMENT_GUIDELINES.md Part 3.1 - "E key triggers E-STOP from any screen"
             */
            case 'E':
                {
                    /* Confirm before activating emergency stop */
                    if (tui_confirm(g_tui.main_win, "EMERGENCY STOP all actuators?")) {
                        result_t r = actuator_manager_emergency_stop(&g_actuator_mgr);
                        if (r == RESULT_OK) {
                            tui_set_status("EMERGENCY STOP - All actuators OFF");
                            LOG_WARNING("EMERGENCY STOP activated via TUI global handler");
                        } else {
                            tui_set_status("E-STOP failed: %s", result_to_string(r));
                            LOG_ERROR("EMERGENCY STOP failed: %d", r);
                        }
                        g_tui.needs_redraw = true;
                    }
                }
                break;

            /* Screen cycling with arrow keys and Tab (no history push) */
            case KEY_LEFT:
                /* Cycle to previous screen in tab order */
                {
                    tui_page_t prev = (g_tui.current_page == 0)
                        ? PAGE_COUNT - 1
                        : g_tui.current_page - 1;
                    cycle_page(prev);
                }
                break;

            case KEY_RIGHT:
            case '\t':  /* Tab key - same as RIGHT */
                /* Cycle to next screen in tab order */
                {
                    tui_page_t next = (g_tui.current_page + 1) % PAGE_COUNT;
                    cycle_page(next);
                }
                break;

            case KEY_BTAB:  /* Shift+Tab - same as LEFT */
                /* Cycle to previous screen */
                {
                    tui_page_t prev = (g_tui.current_page == 0)
                        ? PAGE_COUNT - 1
                        : g_tui.current_page - 1;
                    cycle_page(prev);
                }
                break;

            case KEY_RESIZE:
                handle_resize();
                break;
            case 27:  /* ESC key */
                /*
                 * ESC always goes back one level. At root, show quit confirmation.
                 * This is consistent with IO_CONFIGURATION_UI_SPEC.md requirements.
                 */
                {
                    /* Check if this is a genuine ESC press (not an escape sequence) */
                    nodelay(g_tui.main_win, TRUE);
                    int next_ch = wgetch(g_tui.main_win);
                    nodelay(g_tui.main_win, FALSE);

                    if (next_ch == ERR) {
                        /* Pure ESC key press - navigate back */
                        if (!navigate_back()) {
                            /* At root screen - show quit confirmation */
                            if (tui_confirm(g_tui.main_win, "Exit Water-Treat RTU?")) {
                                g_tui.running = false;
                            }
                        }
                    } else {
                        /* Escape sequence - put back and let ncurses handle */
                        ungetch(next_ch);
                    }
                }
                break;
            default:
                /* Pass to current page */
                if (pages[g_tui.current_page].handle_input) {
                    pages[g_tui.current_page].handle_input(g_tui.main_win, ch);
                    g_tui.needs_redraw = true;
                }
                break;
        }
    }
}

void tui_shutdown(void) {
    /* Mark TUI as inactive BEFORE cleanup so logger stops routing here */
    g_tui.initialized = false;
    g_tui.running = false;

    /* Cleanup current page */
    if (pages[g_tui.current_page].cleanup) {
        pages[g_tui.current_page].cleanup();
    }

    /* Destroy windows */
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

bool tui_is_active(void) {
    /*
     * Returns true if TUI is initialized and should receive log messages.
     * The logger checks this to route messages through the TUI message area
     * instead of writing directly to stdout/stderr (which corrupts the display).
     */
    return g_tui.initialized;
}

void tui_log_message(int level, const char *message) {
    /*
     * Route log messages through TUI instead of direct console write.
     * Messages are stored in a ring buffer and displayed in the status area.
     *
     * This function is called by the logger when tui_is_active() returns true.
     */
    if (!g_tui.initialized || !message) return;

    /* Add to ring buffer */
    tui_msg_entry_t *entry = &g_tui.msg_ring[g_tui.msg_ring_head];
    SAFE_STRNCPY(entry->message, message, TUI_MSG_MAX_LEN);
    entry->level = level;
    entry->timestamp = time(NULL);

    g_tui.msg_ring_head = (g_tui.msg_ring_head + 1) % TUI_MSG_RING_SIZE;
    if (g_tui.msg_ring_count < TUI_MSG_RING_SIZE) {
        g_tui.msg_ring_count++;
    }

    /* Also update status bar for immediate visibility */
    SAFE_STRNCPY(g_tui.status_message, message, sizeof(g_tui.status_message));
    g_tui.status_time = time(NULL);
    g_tui.needs_redraw = true;
}
