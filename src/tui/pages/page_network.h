#ifndef PAGE_NETWORK_H
#define PAGE_NETWORK_H

#include <ncurses.h>

void page_network_init(WINDOW *win);
void page_network_draw(WINDOW *win);
void page_network_input(WINDOW *win, int ch);
void page_network_cleanup(void);

#endif
