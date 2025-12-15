#ifndef PAGE_ALARMS_H
#define PAGE_ALARMS_H

#include <ncurses.h>

void page_alarms_init(WINDOW *win);
void page_alarms_draw(WINDOW *win);
void page_alarms_input(WINDOW *win, int ch);
void page_alarms_cleanup(void);

#endif
