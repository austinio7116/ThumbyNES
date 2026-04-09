/*
 * ThumbyNES — thin wrapper around the vendored peanut_gb / minigb_apu.
 *
 * Mirror of nes_core / sms_core: same five operations, same shape, so
 * the host and device runners can dispatch all three cores through
 * symmetric calls.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native Game Boy resolution. */
#define GBC_SCREEN_W   160
#define GBC_SCREEN_H   144

/* Controller bits — match peanut_gb's JOYPAD_* layout exactly so we
 * can stuff this byte straight into gb.direct.joypad. */
#define GBC_BTN_A       0x01
#define GBC_BTN_B       0x02
#define GBC_BTN_SELECT  0x04
#define GBC_BTN_START   0x08
#define GBC_BTN_RIGHT   0x10
#define GBC_BTN_LEFT    0x20
#define GBC_BTN_UP      0x40
#define GBC_BTN_DOWN    0x80

/* Built-in palettes (4 shades each). */
#define GBC_PALETTE_COUNT  6
#define GBC_PALETTE_GREEN  0
#define GBC_PALETTE_GREY   1
#define GBC_PALETTE_POCKET 2
#define GBC_PALETTE_CREAM  3
#define GBC_PALETTE_BLUE   4
#define GBC_PALETTE_RED    5

/* Initialise the core. `sample_rate` is currently fixed at compile
 * time inside vendored minigb_apu (22050 Hz). The parameter is kept
 * for API symmetry. Returns 0 on success. */
int  gbc_init(int sample_rate);

/* Load a ROM image already resident in memory. The buffer must remain
 * valid for the lifetime of the session — peanut_gb's gb_rom_read_cb
 * reads pages straight out of it. Returns 0 on success, negative on
 * peanut_gb init failure (cast to gb_init_error_e). */
int  gbc_load_rom(const uint8_t *data, size_t len);

/* Run one emulated DMG frame (~17.5 ms wall time at native speed). */
void gbc_run_frame(void);

/* DMG runs at exactly DMG_CLOCK_FREQ / SCREEN_REFRESH_CYCLES =
 * 59.7275 Hz; we round to 60 for pacing. */
int  gbc_refresh_rate(void);

/* Pointer to the most recently rendered frame as 2-bit GB shades
 * (values 0..3), one byte per pixel, stride = GBC_SCREEN_W. */
const uint8_t  *gbc_framebuffer(void);

/* RGB565 palette (4 entries — index by shade). Updated by
 * gbc_set_palette(). */
const uint16_t *gbc_palette_rgb565(void);

/* Switch to one of the built-in palettes (0..GBC_PALETTE_COUNT-1).
 * Cheap — only repopulates the 4-entry lookup. */
void gbc_set_palette(int index);
const char *gbc_palette_name(int index);

/* Set the controller bitmask (GBC_BTN_*). */
void gbc_set_buttons(uint8_t mask);

/* Pull up to `n` int16 mono samples from the most recent frame's
 * audio output. peanut_gb + minigb_apu produce ~369 stereo samples
 * per frame at 22050 Hz; we mix L/R to mono. */
int  gbc_audio_pull(int16_t *out, int n);

/* Battery-backed cart RAM access. Pointer is owned by the wrapper
 * and remains valid until gbc_shutdown(). Returns NULL / 0 if the
 * loaded cart has no battery. */
uint8_t *gbc_battery_ram (void);
size_t   gbc_battery_size(void);

/* Tear down. Frees the cart-ram allocation; the rom buffer is
 * caller-owned and not touched. */
void gbc_shutdown(void);
