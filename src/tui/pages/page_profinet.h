#ifndef PAGE_PROFINET_H
#define PAGE_PROFINET_H

#include <ncurses.h>

void page_profinet_init(WINDOW *win);
void page_profinet_draw(WINDOW *win);
void page_profinet_input(WINDOW *win, int ch);
void page_profinet_cleanup(void);

#endif
