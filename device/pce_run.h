/*
 * ThumbyNES — device-side PC Engine runner.
 *
 * Sibling of nes_run.h / sms_run.h / gb_run.h / md_run.h. Same shape:
 * pick a ROM, drive it through HuExpress, render into the 128×128 LCD
 * framebuffer, return when the user holds MENU.
 *
 * NOT BUILT by default — gated behind THUMBYNES_WITH_PCE in
 * device/CMakeLists.txt. Requires PCE_SCANLINE_RENDER to be functional
 * before this can flip ON; see PCE_PLAN.md §5.
 */
#ifndef THUMBYNES_PCE_RUN_H
#define THUMBYNES_PCE_RUN_H

#include <stdint.h>

#include "nes_picker.h"

int pce_run_rom(const nes_rom_entry *e, uint16_t *fb);

/* Per-cart overclock override peek — see nes_run_clock_override. */
int pce_run_clock_override(const char *rom_name);

#endif
