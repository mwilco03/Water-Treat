/**
 * @file page_login.c
 * @brief Login page implementation
 */

#include "page_login.h"
#include "tui/tui_common.h"
#include "auth/auth.h"
#include "db/database.h"
#include "utils/logger.h"
#include <ncurses.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

/* Login state */
static struct {
    bool initialized;
    int current_field;      /* 0=username, 1=password */
    char username[AUTH_MAX_USERNAME];
    char password[AUTH_MAX_PASSWORD];
    char error_msg[128];
    bool show_error;
    int cursor_pos;
    int attempts;
    bool confirm_exit;
    time_t confirm_exit_time;
} g_login = {0};

/* Get database via TUI common */

/* Water droplet ASCII art */
static const char *WATER_LOGO[] = {
    "        ██        ",
    "       ████       ",
    "      ██████      ",
    "     ████████     ",
    "    ██████████    ",
    "   ████████████   ",
    "   ████████████   ",
    "    ██████████    ",
    "     ████████     ",
    "       ████       ",
    NULL
};

static const char *TITLE_ART[] = {
    " _    _       _              _____              _   ",
    "| |  | |     | |            |_   _|            | |  ",
    "| |  | | __ _| |_ ___ _ __    | |_ __ ___  __ _| |_ ",
    "| |/\\| |/ _` | __/ _ \\ '__|   | | '__/ _ \\/ _` | __|",
    "\\  /\\  / (_| | ||  __/ |      | | | |  __/ (_| | |_ ",
    " \\/  \\/ \\__,_|\\__\\___|_|      \\_/_|  \\___|\\__,_|\\__|",
    NULL
};

/* ============================================================================
 * Drawing Functions
 * ========================================================================== */

static void draw_centered(int y, const char *text) {
    int x = (COLS - strlen(text)) / 2;
    if (x < 0) x = 0;
    mvprintw(y, x, "%s", text);
}

static void draw_logo(int start_y) {
    attron(COLOR_PAIR(4) | A_BOLD);  /* Blue/cyan */
    for (int i = 0; WATER_LOGO[i] != NULL; i++) {
        draw_centered(start_y + i, WATER_LOGO[i]);
    }
    attroff(COLOR_PAIR(4) | A_BOLD);
}

static void draw_title(int start_y) {
    attron(COLOR_PAIR(4) | A_BOLD);
    for (int i = 0; TITLE_ART[i] != NULL; i++) {
        draw_centered(start_y + i, TITLE_ART[i]);
    }
    attroff(COLOR_PAIR(4) | A_BOLD);
}

static void draw_login_box(int y, int x, int h, int w) {
    /* Draw border */
    attron(COLOR_PAIR(1));
    for (int i = 0; i < h; i++) {
        mvhline(y + i, x, ' ', w);
    }

    /* Box outline */
    mvhline(y, x, ACS_HLINE, w);
    mvhline(y + h - 1, x, ACS_HLINE, w);
    mvvline(y, x, ACS_VLINE, h);
    mvvline(y, x + w - 1, ACS_VLINE, h);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);

    /* Title */
    mvprintw(y, x + (w - 9) / 2, "[ Login ]");
    attroff(COLOR_PAIR(1));
}

static void draw_field(int y, int x, const char *label, const char *value,
                       bool selected, bool is_password) {
    mvprintw(y, x, "%s: ", label);

    if (selected) {
        attron(A_REVERSE);
    }

    /* Field background */
    mvhline(y, x + 12, ' ', 24);

    /* Field value */
    if (is_password) {
        /* Show asterisks for password */
        for (size_t i = 0; i < strlen(value) && i < 24; i++) {
            mvaddch(y, x + 12 + i, '*');
        }
    } else {
        mvprintw(y, x + 12, "%-24.24s", value);
    }

    if (selected) {
        attroff(A_REVERSE);
        /* Show cursor */
        int cursor_x = x + 12 + g_login.cursor_pos;
        mvchgat(y, cursor_x, 1, A_UNDERLINE | A_BOLD, 0, NULL);
    }
}

/* ============================================================================
 * Public API
 * ========================================================================== */

void page_login_init(void) {
    memset(&g_login, 0, sizeof(g_login));
    g_login.initialized = true;
}

void page_login_draw(void) {
    clear();

    int logo_y = 2;
    int title_y = logo_y + 11;
    int box_y = title_y + 8;
    int box_x = (COLS - 44) / 2;
    int box_h = 10;
    int box_w = 44;

    /* Draw logo and title */
    draw_logo(logo_y);
    draw_title(title_y);

    /* Draw login box */
    draw_login_box(box_y, box_x, box_h, box_w);

    /* Draw fields */
    int field_x = box_x + 4;
    int field_y = box_y + 2;

    draw_field(field_y, field_x, "Username", g_login.username,
               g_login.current_field == 0, false);
    draw_field(field_y + 2, field_x, "Password", g_login.password,
               g_login.current_field == 1, true);

    /* Draw buttons hint */
    attron(A_DIM);
    //mvprintw(field_y + 5, field_x, "TAB: Next field | ENTER: Login | ESC: Quit");
    if (g_login.confirm_exit && (time(NULL) - g_login.confirm_exit_time) <= 2) {
        mvprintw(field_y + 5, field_x, "Press ESC again to quit");
    } else {
        mvprintw(field_y + 5, field_x, "TAB: Next field | ENTER: Login | ESC: Quit");
    }
    
    attroff(A_DIM);

    /* Error message */
    if (g_login.show_error && strlen(g_login.error_msg) > 0) {
        attron(COLOR_PAIR(3) | A_BOLD);  /* Red/yellow for error */
        draw_centered(box_y + box_h + 1, g_login.error_msg);
        attroff(COLOR_PAIR(3) | A_BOLD);
    }

    /* Footer hints */
    attron(A_DIM);
    draw_centered(LINES - 3, "PROFINET Water Treatment RTU Monitor");
    draw_centered(LINES - 2, "Default: admin / H2OhYeah!");
    attroff(A_DIM);

    refresh();
}

bool page_login_input(int ch) {
    char *current_buf = (g_login.current_field == 0) ?
                        g_login.username : g_login.password;
    size_t max_len = (g_login.current_field == 0) ?
                     sizeof(g_login.username) : sizeof(g_login.password);
    size_t len = strlen(current_buf);

    switch (ch) {
        case 27:  /* ESC */
            return false;  /* Signal quit */

        case '\t':
        case KEY_DOWN:
            g_login.current_field = (g_login.current_field + 1) % 2;
            g_login.cursor_pos = strlen((g_login.current_field == 0) ?
                                        g_login.username : g_login.password);
            g_login.show_error = false;
            break;

        case KEY_UP:
            g_login.current_field = (g_login.current_field + 1) % 2;
            g_login.cursor_pos = strlen((g_login.current_field == 0) ?
                                        g_login.username : g_login.password);
            g_login.show_error = false;
            break;

        case '\n':
        case KEY_ENTER:
            /* Attempt login */
            if (strlen(g_login.username) == 0) {
                SAFE_STRNCPY(g_login.error_msg, "Username required",
                             sizeof(g_login.error_msg));
                g_login.show_error = true;
                g_login.current_field = 0;
            } else if (strlen(g_login.password) == 0) {
                SAFE_STRNCPY(g_login.error_msg, "Password required",
                             sizeof(g_login.error_msg));
                g_login.show_error = true;
                g_login.current_field = 1;
            } else {
                database_t *db = tui_get_database();
                result_t result = auth_login(db, g_login.username,
                                            g_login.password);
                if (result == RESULT_OK) {
                    return true;  /* Success! */
                } else {
                    g_login.attempts++;
                    if (g_login.attempts >= 3) {
                        snprintf(g_login.error_msg, sizeof(g_login.error_msg),
                                "Invalid credentials (%d attempts). Hint: H2OhYeah!",
                                g_login.attempts);
                    } else {
                        SAFE_STRNCPY(g_login.error_msg,
                                    "Invalid username or password",
                                    sizeof(g_login.error_msg));
                    }
                    g_login.show_error = true;
                    /* Clear password */
                    memset(g_login.password, 0, sizeof(g_login.password));
                    g_login.current_field = 1;
                    g_login.cursor_pos = 0;
                }
            }
            break;

        case KEY_BACKSPACE:
        case 127:
        case '\b':
            if (g_login.cursor_pos > 0) {
                memmove(&current_buf[g_login.cursor_pos - 1],
                        &current_buf[g_login.cursor_pos],
                        len - g_login.cursor_pos + 1);
                g_login.cursor_pos--;
            }
            g_login.show_error = false;
            break;

        case KEY_DC:  /* Delete */
            if (g_login.cursor_pos < (int)len) {
                memmove(&current_buf[g_login.cursor_pos],
                        &current_buf[g_login.cursor_pos + 1],
                        len - g_login.cursor_pos);
            }
            g_login.show_error = false;
            break;

        case KEY_LEFT:
            if (g_login.cursor_pos > 0) {
                g_login.cursor_pos--;
            }
            break;

        case KEY_RIGHT:
            if (g_login.cursor_pos < (int)len) {
                g_login.cursor_pos++;
            }
            break;

        case KEY_HOME:
            g_login.cursor_pos = 0;
            break;

        case KEY_END:
            g_login.cursor_pos = len;
            break;

        default:
            /* Printable character */
            if (isprint(ch) && len < max_len - 1) {
                memmove(&current_buf[g_login.cursor_pos + 1],
                        &current_buf[g_login.cursor_pos],
                        len - g_login.cursor_pos + 1);
                current_buf[g_login.cursor_pos] = ch;
                g_login.cursor_pos++;
                g_login.show_error = false;
            }
            break;
    }

    return true;  /* Continue */
}

void page_login_cleanup(void) {
    /* Clear sensitive data */
    memset(g_login.password, 0, sizeof(g_login.password));
    g_login.initialized = false;
}

result_t page_login_run(void) {
    page_login_init();

    bool running = true;
    bool success = false;

    while (running) {
        page_login_draw();
        int ch = getch();
    
        if (ch == 27) {  /* ESC */
            time_t now = time(NULL);
            if (g_login.confirm_exit && (now - g_login.confirm_exit_time) <= 2) {
                running = false; /* confirmed */
            } else {
                g_login.confirm_exit = true;
                g_login.confirm_exit_time = now;
            }
            continue;
        } else {
            /* Any other key cancels pending exit confirmation */
            g_login.confirm_exit = false;
        }
    
        bool result = page_login_input(ch);
        if (ch == '\n' || ch == KEY_ENTER) {
            if (auth_is_logged_in()) {
                success = true;
                running = false;
            }
        }
        (void)result;
    }

    page_login_cleanup();
    return success ? RESULT_OK : RESULT_ERROR;
}

bool page_login_required(void) {
    return !auth_is_logged_in();
}
