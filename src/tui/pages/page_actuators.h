/**
 * @file page_actuators.h
 * @brief Actuator management and manual control page
 */

#ifndef PAGE_ACTUATORS_H
#define PAGE_ACTUATORS_H

#include <ncurses.h>

void page_actuators_init(WINDOW *win);
void page_actuators_draw(WINDOW *win);
void page_actuators_input(WINDOW *win, int ch);
void page_actuators_cleanup(void);

#endif /* PAGE_ACTUATORS_H */
