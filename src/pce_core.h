/*
 * ThumbyNES — thin wrapper around the vendored HuExpress core (PC Engine
 * / TurboGrafx-16 HuCard-only).
 *
 * Mirrors sms_core.h / gb_core.h / md_core.h shape so host and device
 * runners dispatch the same way across all five cores.
 *
 * Scope: HuCard only. No CD-ROM², no Arcade Card, no SuperGrafx, no
 * Six-button pad for v1. See PCE_PLAN.md §10.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native source dimensions. PC Engine VDC is programmable — modes
 * range from 256x224 (common) up to 352x242. HuExpress renders into
 * an over-sized 600x368 palette-indexed bitmap to absorb sprite writes
 * that would otherwise walk off the active area. We expose that real
 * stride via PCEC_PITCH so callers indexing by row stay in bounds;
 * the active viewport sits at (0, 0) of the `pcec_framebuffer()` ptr
 * (which is already offset past the top/left padding). */
#define PCEC_BITMAP_W   600
#define PCEC_BITMAP_H   240
#define PCEC_PITCH      PCEC_BITMAP_W   /* 1 byte per pixel */

/* Common active viewport — most HuCards. Runtime-queryable via pcec_viewport. */
#define PCEC_DEFAULT_W  256
#define PCEC_DEFAULT_H  224

/* Region selectors passed to pcec_init. 0 = auto-detect from header. */
#define PCEC_REGION_AUTO    0
#define PCEC_REGION_JP      1   /* PC Engine */
#define PCEC_REGION_US      2   /* TurboGrafx-16 */

/* Controller bitmask — thumby_buttons → pcec_set_buttons translation
 * mirrors HuC6280 joypad register layout so the core can consume it
 * directly without a second shuffle. */
#define PCEC_BTN_I       0x01  /* A */
#define PCEC_BTN_II      0x02  /* B */
#define PCEC_BTN_SELECT  0x04  /* LB */
#define PCEC_BTN_RUN     0x08  /* MENU */
#define PCEC_BTN_UP      0x10
#define PCEC_BTN_RIGHT   0x20
#define PCEC_BTN_DOWN    0x40
#define PCEC_BTN_LEFT    0x80

/* ---- US-encoded ROM helpers -----------------------------------------
 *
 * TurboGrafx-16 (US) HuCards are stored with every byte bit-reversed
 * relative to native PC Engine (JP) format. HuExpress's upstream
 * InitPCE detected and rewrote the buffer in place, which is not safe
 * on our device build where the ROM lives in read-only XIP flash.
 *
 * These two helpers let the caller decide *where* to put the writable
 * buffer: on host it's a malloc'd RAM shadow, on device it's the flash
 * file itself updated via FatFs before `pcec_load_rom` ever sees it.
 * The core is then handed a buffer that is always in native encoding.
 *
 * Both helpers are pure — no globals, no allocation. The detector is
 * self-idempotent: running it on already-decrypted content returns
 * false, so a retry after a crash never decrypts twice.
 */

/* True if `data` looks like a US-encoded TurboGrafx-16 HuCard.
 * Checks the reset-vector high byte heuristic (ROM[0x1FFF] < 0xE0)
 * combined with a CRC lookup in HuExpress's kKnownRoms table. */
bool pcec_rom_is_us_encoded(const uint8_t *data, size_t len);

/* Bit-reverse every byte of `data` in place. Involution — do not
 * call twice. Safe to run sector-by-sector (each byte is independent). */
void pcec_rom_decrypt_us(uint8_t *data, size_t len);

/* Initialise the core. `region` picks JP/US/AUTO; `sample_rate` is the
 * PSG mixer output rate (8000..48000). Returns 0 on success. Call once
 * before pcec_load_rom. Idempotent; re-calling releases prior state. */
int  pcec_init(int region, int sample_rate);

/* Load a HuCard ROM image already resident in memory. Buffer must
 * remain valid for the lifetime of the session (core reads pages out
 * of it in place). Handles 384 KB / 512 KB / 768 KB / 1 MB carts and
 * the standard 512-byte header strip. Returns 0 on success. */
int  pcec_load_rom(const uint8_t *data, size_t len);

/* Run one emulated frame (one complete vsync cycle). */
void pcec_run_frame(void);

/* Effective refresh rate (PCE is always 60 Hz NTSC; PAL HuCards are
 * extremely rare). Call after pcec_load_rom. */
int  pcec_refresh_rate(void);

/* Active viewport inside the PCEC_BITMAP_W × PCEC_BITMAP_H bitmap.
 * Depends on VDC mode selected by the running game. Typical values:
 *   Bonk, Soldier Blade .... (0, 0, 256, 224)
 *   R-Type ................. (0, 0, 256, 240)
 *   Art of Fighting ........ (0, 0, 336, 240)  [we clip at 256]
 */
void pcec_viewport(int *x, int *y, int *w, int *h);

/* Pointer to the most recently rendered frame as 8-bit palette
 * indices, stride = PCEC_PITCH. Stable between pcec_run_frame calls. */
const uint8_t  *pcec_framebuffer(void);

/* 512-entry RGB565 palette (PCE VCE is 9-bit RGB per entry, 512 entries;
 * 256 background + 256 sprite). Refreshed by pcec_run_frame when VCE
 * writes occurred. */
const uint16_t *pcec_palette_rgb565(void);

/* Write the Player 1 pad (PCEC_BTN_* mask). Two-player pads and the
 * six-button Avenue Pad 6 are out of scope for v1. */
void pcec_set_buttons(uint16_t mask);

/* Skip the next frame's VDC rendering to recover CPU budget. The VDC
 * registers still advance; only the line renderer is elided. Used by
 * the frame-pacing logic in device/pce_run.c when falling behind. */
void pcec_set_skip_render(int skip);

/* Bind the 128×128 RGB565 output framebuffer + scale/blend mode the
 * scanline renderer should write into. Call once before each
 * pcec_run_frame (the binding is cheap; the runners call it per
 * frame in case the menu toggles scale/blend or LB-pan moves).
 *
 * scale_mode: 0 = FIT  (2:1, letterboxed Y).
 *             1 = FILL (preserve aspect via Y scale, pannable in X).
 *             2 = CROP (1:1 native 128×128, pannable in X and Y).
 * blend:      0 = nearest-neighbour, 1 = 2×2 box average (FIT/FILL).
 * pan_x/y:    source-pixel offsets (CROP/FILL). Clamped internally.
 *
 * pcec_run_frame composites BG + sprites DIRECTLY into `lcd_fb` —
 * there is no intermediate full framebuffer. Peak per-session RAM
 * cost of the renderer is ~128 bytes of visible-sprite scan list.
 *
 * On host builds (pcehost / pcebench) the scale target may be NULL;
 * in that case the core still runs but no pixels are written. Those
 * binaries use pcec_framebuffer() instead to pull an already-rendered
 * target for SDL display. */
void pcec_set_scale_target(uint16_t *lcd_fb, int scale_mode, int blend,
                            int pan_x, int pan_y);

/* Pull n int16 mono samples produced by the most recent frame. Returns
 * the count actually written (= sndrate / 60 + 1 per frame typically).
 * HuExpress mixes the 6 PSG channels internally; we sum to mono. */
int  pcec_audio_pull(int16_t *out, int n);

/* Battery-backed RAM (BRAM) — 2 KB window used by save-capable
 * HuCards (Dungeon Explorer, Neutopia II, Legendary Axe II, Ys). The
 * pointer is always valid after pcec_init; persistence is the caller's
 * decision based on whether the cart header flags BRAM presence. */
uint8_t *pcec_battery_ram (void);
size_t   pcec_battery_size(void);

/* Save / load runtime state to a FAT sidecar file (absolute path,
 * e.g. "/Bonk.sta"). Format is custom to HuExpress — no upstream
 * spec exists — but compatible with the thumby_state_bridge FatFs
 * shim that SMS/NES/MD use. Returns 0 on success. */
int pcec_save_state(const char *path);
int pcec_load_state(const char *path);

/* Tear down. Releases heap allocations so a subsequent pcec_init
 * starts from a clean slate. */
void pcec_shutdown(void);
