/*
 * ThumbyNES — device-side Game Boy runner.
 *
 * Sibling of nes_run.h / sms_run.h. Same shape: pick a ROM, drive it
 * through peanut_gb, render into the 128x128 LCD framebuffer, return
 * when the user holds MENU.
 */
#ifndef THUMBYNES_GB_RUN_H
#define THUMBYNES_GB_RUN_H

#include <stdint.h>

#include "nes_picker.h"

int gb_run_rom(const nes_rom_entry *e, uint16_t *fb);

/* Per-cart overclock override peek — see nes_run_clock_override. */
int gb_run_clock_override(const char *rom_name);

#endif
