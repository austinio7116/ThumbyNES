/*
 * ThumbyNES — thin wrapper around the vendored PicoDrive core (MD/Genesis).
 *
 * Mirrors sms_core.h shape so the host and device runners can dispatch
 * either core through symmetric calls.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native MD display — line renderer emits up to 320x240 visible lines.
 * 320 wide covers H40 mode; H32 (256) is centred inside the same buffer
 * by PicoDrive itself (PDRAW_BORDER_32). Vertical is 224 (NTSC 28-row)
 * or 240 (PAL 30-row) — we size the buffer for worst case. */
#define MDC_MAX_W   320
#define MDC_MAX_H   240

/* Pixel format we ask PicoDrive to render. 16-bit RGB565 direct-to-host,
 * no palette-lookup step on our side — Pico does it with CRAM->RGB555. */

/* Controller mask. MD pad bit layout is MXYZ SACB RLDU; we mirror it
 * here with stable aliases for host + device button wiring. */
#define MDC_BTN_UP       0x0001
#define MDC_BTN_DOWN     0x0002
#define MDC_BTN_LEFT     0x0004
#define MDC_BTN_RIGHT    0x0008
#define MDC_BTN_B        0x0010
#define MDC_BTN_C        0x0020
#define MDC_BTN_A        0x0040
#define MDC_BTN_START    0x0080
#define MDC_BTN_Z        0x0100
#define MDC_BTN_Y        0x0200
#define MDC_BTN_X        0x0400
#define MDC_BTN_MODE     0x0800

/* Region override — passed to mdc_init. 0 means auto-detect from ROM. */
#define MDC_REGION_AUTO     0
#define MDC_REGION_JP_NTSC  1
#define MDC_REGION_JP_PAL   2
#define MDC_REGION_US       4
#define MDC_REGION_EU       8

/* Initialise the core. `region` is an MDC_REGION_* selector.
 * `sample_rate` is the PSG+FM audio rate (11025..48000). Returns 0 on
 * success. Must be called once per process before mdc_load_rom. */
int  mdc_init(int region, int sample_rate);

/* Load a ROM image already resident in memory. Default path copies the
 * ROM into a malloc'd buffer so the caller can free its source. Use
 * mdc_load_rom_xip() on device to skip the copy when the source is
 * already in flash. `data` is treated as a plain un-interleaved
 * Genesis dump (.md / .bin / .gen); SMD interleaved headers are NOT
 * supported here. Returns 0 on success. */
int  mdc_load_rom(const uint8_t *data, size_t len);

/* Load a ROM image borrowed from a stable XIP-flash (or otherwise
 * lifetime-safe) pointer. No copy. Caller must keep `data` valid until
 * mdc_shutdown(). The buffer must have at least 4 bytes of writable
 * scratch after `len` — PicoCartInsert writes a safety "jump-back"
 * opcode at rom[len..len+3]. On device, the picker's XIP region is
 * read-only flash, so this function writes the safety opcode into a
 * tiny RAM trampoline that replaces the last page of `data` internally.
 * Returns 0 on success. */
int  mdc_load_rom_xip(const uint8_t *data, size_t len);

/* Run one emulated frame (call PicoFrame). */
void mdc_run_frame(void);

/* Effective refresh rate (50 PAL / 60 NTSC) after mdc_load_rom. */
int  mdc_refresh_rate(void);

/* Active display dimensions for the most recent frame. For H32 mode
 * (w=256) PicoDrive centres the active area within a 320-wide buffer;
 * we report the *rendered* inner rectangle here so the host runner /
 * device blit can crop without re-parsing VDP state. */
void mdc_viewport(int *x, int *y, int *w, int *h);

/* Pointer to the most recently rendered frame, as uint16 RGB565,
 * stride = MDC_MAX_W * 2 bytes. Valid until the next mdc_run_frame. */
const uint16_t *mdc_framebuffer(void);

/* Set Player 1 controller mask (MDC_BTN_*). */
void mdc_set_buttons(uint16_t mask);

/* Pull n int16 mono samples produced by the most recent frame. Returns
 * the count actually written. PicoDrive produces (sndrate / fps)
 * samples per frame; the wrapper mixes the stereo stream to mono. */
int  mdc_audio_pull(int16_t *out, int n);

/* Battery-backed cart SRAM. Returns NULL / 0 if the loaded cart
 * doesn't declare SRAM in its header. */
uint8_t *mdc_battery_ram (void);
size_t   mdc_battery_size(void);

/* Save / load runtime state to a sidecar file (absolute host or FAT
 * path). Routes through PicoState, which uses the thumby_state_bridge
 * FatFs shim on the device build. Returns 0 on success. */
int mdc_save_state(const char *path);
int mdc_load_state(const char *path);

/* Tear down. Frees frame buffer and PicoDrive-owned memory. */
void mdc_shutdown(void);
