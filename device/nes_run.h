/*
 * ThumbyNES — device-side ROM runner.
 *
 * Loads a ROM from the FAT volume, hands it to the Nofrendo core,
 * and runs the main video / audio / input loop until the user
 * holds MENU. Uses the global framebuffer + LCD driver from
 * nes_device_main.c.
 */
#ifndef THUMBYNES_RUN_H
#define THUMBYNES_RUN_H

#include <stdint.h>

#include "nes_picker.h"

/* Run a ROM described by a picker entry. `fb` is the 128×128
 * RGB565 framebuffer to draw into. Returns 0 on clean exit
 * (MENU held), nonzero on load error. The entry's pal_hint is
 * used as the default region if the per-ROM cfg doesn't yet
 * exist. */
int nes_run_rom(const nes_rom_entry *e, uint16_t *fb);

#endif
