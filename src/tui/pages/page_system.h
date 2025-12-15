#ifndef PAGE_SYSTEM_H
#define PAGE_SYSTEM_H

#include <ncurses.h>

void page_system_init(WINDOW *win);
void page_system_draw(WINDOW *win);
void page_system_input(WINDOW *win, int ch);
void page_system_cleanup(void);

#endif
