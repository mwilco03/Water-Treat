#ifndef PAGE_MODBUS_H
#define PAGE_MODBUS_H

#include <ncurses.h>

void page_modbus_init(WINDOW *win);
void page_modbus_draw(WINDOW *win);
void page_modbus_input(WINDOW *win, int ch);
void page_modbus_cleanup(void);

#endif
