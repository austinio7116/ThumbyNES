/*
 * ThumbyNES — Thumby Color physical button reader.
 */
#ifndef THUMBYNES_BUTTONS_H
#define THUMBYNES_BUTTONS_H

#include <stdint.h>

void    nes_buttons_init(void);
uint8_t nes_buttons_read(void);   /* PICO-8 6-bit mask: LRUDOX */
int     nes_buttons_menu_pressed(void);

#endif
