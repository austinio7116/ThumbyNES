/*
 * ThumbyNES — picker thumbnail / icon helpers.
 *
 * Procedural platform icons (drawn from code, no bitmap assets) and
 * a tiny RGB565 thumbnail loader for the .scr32 / .scr64 sidecars
 * that the runners write on MENU+A.
 */
#ifndef THUMBYNES_THUMB_H
#define THUMBYNES_THUMB_H

#include <stdbool.h>
#include <stdint.h>

#include "nes_picker.h"   /* ROM_SYS_* */

/* Tab / system icon styles. STAR is used for the favorites tab. */
#define ICON_SYS_NES   ROM_SYS_NES
#define ICON_SYS_SMS   ROM_SYS_SMS
#define ICON_SYS_GG    ROM_SYS_GG
#define ICON_SYS_GB    ROM_SYS_GB
#define ICON_SYS_MD    ROM_SYS_MD
#define ICON_SYS_PCE   ROM_SYS_PCE
#define ICON_SYS_STAR  255

/* Draw a 12×8 procedural tab icon at (x, y) into the 128×128
 * framebuffer. `tint` is used as the foreground; the icon background
 * is left transparent (caller fills the tab cell behind it). */
void nes_thumb_icon(uint16_t *fb, int x, int y, uint8_t which,
                     uint16_t tint);

/* Draw a procedural placeholder thumbnail (used when no .scrNN
 * sidecar is on disk for a ROM). `size` must be 32 or 64. The
 * placeholder is the matching platform icon centred in a coloured
 * panel — recognisable as "no screenshot yet, system X". */
void nes_thumb_placeholder(uint16_t *fb, int x, int y, int size,
                            uint8_t system);

/* Try to load a saved screenshot for `rom_name` at the given size
 * (32 or 64). Returns true on success and renders the thumbnail at
 * (x, y) directly into `fb`. Returns false (and renders nothing)
 * if the sidecar is missing or the wrong size. Reads from
 * /<basename>.scrNN. */
bool nes_thumb_draw(uint16_t *fb, int x, int y, int size,
                     const char *rom_name);

/* Save a screenshot for `rom_name`. Downscales the live 128×128
 * framebuffer `src` into both a 32×32 .scr32 and a 64×64 .scr64
 * sidecar via box averaging. Returns 0 on success, nonzero on FAT
 * write failure. */
int  nes_thumb_save(const uint16_t *src, const char *rom_name);

#endif
