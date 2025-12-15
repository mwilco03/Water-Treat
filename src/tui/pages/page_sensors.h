#ifndef PAGE_SENSORS_H
#define PAGE_SENSORS_H

#include <ncurses.h>

void page_sensors_init(WINDOW *win);
void page_sensors_draw(WINDOW *win);
void page_sensors_input(WINDOW *win, int ch);
void page_sensors_cleanup(void);

#endif
