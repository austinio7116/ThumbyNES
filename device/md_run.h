/*
 * ThumbyNES — device-side Mega Drive / Genesis runner.
 *
 * Sibling of sms_run.h / nes_run.h / gb_run.h. Same shape: pick a ROM,
 * drive it through PicoDrive, render into the 128x128 LCD framebuffer,
 * return when the user holds MENU.
 */
#ifndef THUMBYNES_MD_RUN_H
#define THUMBYNES_MD_RUN_H

#include <stdint.h>

#include "nes_picker.h"

int md_run_rom(const nes_rom_entry *e, uint16_t *fb);

/* Per-cart overclock override peek — see nes_run_clock_override. */
int md_run_clock_override(const char *rom_name);

#endif
