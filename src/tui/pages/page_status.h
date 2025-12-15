#ifndef PAGE_STATUS_H
#define PAGE_STATUS_H

#include <ncurses.h>

void page_status_init(WINDOW *win);
void page_status_draw(WINDOW *win);
void page_status_input(WINDOW *win, int ch);
void page_status_cleanup(void);

#endif
