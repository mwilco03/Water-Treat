#ifndef PAGE_LOGGING_H
#define PAGE_LOGGING_H

#include <ncurses.h>

void page_logging_init(WINDOW *win);
void page_logging_draw(WINDOW *win);
void page_logging_input(WINDOW *win, int ch);
void page_logging_cleanup(void);

#endif
