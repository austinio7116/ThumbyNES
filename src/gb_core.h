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

/* Chained-ROM variant — used when the cart file is fragmented on
 * flash and can't be presented as a single contiguous XIP pointer.
 * `cluster_ptrs[i]` is the XIP address of cluster i's first byte;
 * `cluster_shift` and `cluster_mask` describe the cluster geometry
 * (typical values 12 and 0xFFF for 4 KB FAT clusters). The array
 * must remain valid for the whole session — peanut_gb reads through
 * it on every ROM byte access. Returns 0 on success. */
int  gbc_load_rom_chain(const uint8_t * const *cluster_ptrs,
                         uint32_t cluster_shift,
                         uint32_t cluster_mask,
                         size_t   len);

/* Run one emulated DMG frame (~17.5 ms wall time at native speed). */
void gbc_run_frame(void);

/* DMG runs at exactly DMG_CLOCK_FREQ / SCREEN_REFRESH_CYCLES =
 * 59.7275 Hz; we round to 60 for pacing. */
int  gbc_refresh_rate(void);

/* Pointer to the most recently rendered frame as fully-resolved
 * RGB565 pixels, one u16 per pixel, stride = GBC_SCREEN_W. The core
 * converts both DMG (4-entry palette) and CGB (per-pixel CGB palette
 * RAM) into RGB565 inside the line callback, so the runner's scaler
 * is a pure word copy — no palette indirection at blit time. */
const uint16_t *gbc_framebuffer(void);

/* True when the most-recently-loaded cart has the CGB flag set in
 * its cartridge header (byte 0x143 == 0x80 or 0xC0). DMG palette
 * selection is a no-op on a CGB cart. */
bool gbc_is_cgb_cart(void);

/* DMG-only: pointer to a per-pixel shade-index buffer (0..3 values,
 * one u8 per pixel, stride = GBC_SCREEN_W). Populated alongside the
 * RGB565 framebuffer for DMG carts so the scaler can blend in
 * palette-index space rather than blending resolved RGB565 colours
 * — the latter interpolates between non-collinear palette entries
 * and shifts hue (classic Nintendo-green shade 0 and shade 2 differ
 * enough in their blue component that a linear RGB blend reads as
 * teal-shifted). Returns NULL on CGB carts, where per-pixel CGB
 * palette resolution happens per-pixel in the line callback and
 * there is no meaningful shade index to expose. */
const uint8_t *gbc_shade_buffer(void);

/* RGB565 palette (4 entries — index by shade). Updated by
 * gbc_set_palette(). Only meaningful for DMG carts. */
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

/* MBC3 cart RTC support. Pokemon Crystal/Gold/Silver and a handful of
 * other GBC carts have a real-time clock built into the cartridge
 * (used for berry growth, day-night cycles, time-of-day-only
 * encounters). peanut_gb already implements the cart-side RTC bytes
 * + the gb_tick_rtc / gb_set_rtc primitives; the wrapper exposes
 * them here so the device runner can drive them off the BM8563. No-op
 * if no cart is loaded.
 *
 * gbc_set_rtc(): seed the cart RTC from a struct tm — typically called
 * once at game load with the wall-clock time read from BM8563. Pokemon
 * stores its "RTC last seen" inside cart RAM, so as long as cart_rtc
 * tracks real wall time, the elapsed-time math the game does on boot
 * works whether the BM8563 keeps time across power-off or not.
 *
 * gbc_tick_rtc(): advance the cart RTC by one second. Call once per
 * real-world second from the runner's frame loop. */
struct tm;
void gbc_set_rtc(const struct tm *t);
void gbc_tick_rtc(void);

/* Copy current cart_rtc[5] bytes into `out` (must be at least 5
 * bytes). Used to capture cart_rtc state for sidecar persistence. */
void gbc_peek_cart_rtc(uint8_t out[5]);

/* Write 5 raw bytes into peanut_gb's cart_rtc. Used to restore from
 * a sidecar file, bypassing the tm-based gbc_set_rtc API. */
void gbc_poke_cart_rtc(const uint8_t in[5]);

/* Save / load runtime state to a sidecar file (absolute FAT path,
 * e.g. "/Tetris.sta"). peanut_gb's struct gb_s plus minigb_apu_ctx
 * is small enough (~17 KB total) to memcpy whole — we just write
 * both blobs to disk and reverse on load. Returns 0 on success.
 *
 * V2 file format also embeds the wall-clock unix seconds at save
 * moment; on load the runner uses (now - saved) to advance the
 * cart_rtc so MBC3 carts (Pokemon Crystal/Gold/Silver) track real
 * elapsed wall clock across the time a state was sitting on disk.
 *
 * gbc_save_state(): pass the current wall-clock unix-seconds via
 * `wall_clock_unix_secs` (or 0 if unavailable — load will then
 * skip the advance).
 *
 * gbc_load_state(): writes the saved wall-clock unix-seconds back
 * via `*out_saved_wall_unix_secs` (or 0 for V1 files / unavailable);
 * pass NULL if you don't need it. */
int gbc_save_state(const char *path, int64_t wall_clock_unix_secs);
int gbc_load_state(const char *path, int64_t *out_saved_wall_unix_secs);

/* Tear down. Frees the cart-ram allocation; the rom buffer is
 * caller-owned and not touched. */
void gbc_shutdown(void);
