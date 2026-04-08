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

/* Run a ROM. `name` is a base file name in / on the FAT volume.
 * `fb` is the 128×128 RGB565 framebuffer to draw into. Returns 0
 * on clean exit (MENU held), nonzero on load error. */
int nes_run_rom(const char *name, uint16_t *fb);

#endif
