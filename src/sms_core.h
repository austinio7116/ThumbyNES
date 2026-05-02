/*
 * ThumbyNES — thin wrapper around the vendored smsplus core (SMS / GG).
 *
 * Mirrors nes_core.h shape so the host and device runners can dispatch
 * either core through symmetric calls.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native source dimensions for each console. The wrapper always renders
 * SMS-sized; for Game Gear the active viewport is a centred sub-rect. */
#define SMSC_SMS_W   256
#define SMSC_SMS_H   192
#define SMSC_GG_W    160
#define SMSC_GG_H    144

/* Allocated bitmap is always SMS_W * SMS_H bytes (palette indices). */
#define SMSC_BITMAP_W   SMSC_SMS_W
#define SMSC_BITMAP_H   SMSC_SMS_H

/* Console selectors (passed to smsc_init). 0 = auto-detect from ROM. */
#define SMSC_SYS_AUTO   0
#define SMSC_SYS_SMS    2  /* force SMS2 */
#define SMSC_SYS_GG     3  /* force Game Gear */
#define SMSC_SYS_GGMS   4  /* GG running an SMS-mode cart */

/* Controller bits — symmetrical with nes_core for ergonomic dispatch. */
#define SMSC_BTN_BUTTON1   0x01  /* B  / "1" */
#define SMSC_BTN_BUTTON2   0x02  /* A  / "2" */
#define SMSC_BTN_PAUSE     0x04  /* SMS Pause */
#define SMSC_BTN_START     0x08  /* GG  Start */
#define SMSC_BTN_UP        0x10
#define SMSC_BTN_DOWN      0x20
#define SMSC_BTN_LEFT      0x40
#define SMSC_BTN_RIGHT     0x80

/* Initialise the core. `console` selects auto/SMS/GG/GGMS. `sample_rate`
 * is the audio rate (8000..48000). Returns 0 on success. Must be called
 * before smsc_load_rom. Safe to call once per process. */
int  smsc_init(int console, int sample_rate);

/* Load a ROM image already resident in memory. Buffer must remain valid
 * for the lifetime of the session — smsplus reads pages out of it. The
 * buffer should be at least 16 KB and ideally 16 KB aligned in length
 * (smsplus expects whole pages). Returns 0 on success. */
int  smsc_load_rom(const uint8_t *data, size_t len);

/* Run one emulated frame. */
void smsc_run_frame(void);

/* Effective refresh rate (50 PAL / 60 NTSC) — call after smsc_load_rom. */
int  smsc_refresh_rate(void);

/* True if the loaded cart is Game Gear (smaller viewport). */
bool smsc_is_gg(void);

/* Active viewport inside the SMSC_BITMAP_W x SMSC_BITMAP_H bitmap. For
 * SMS this is 0,0,256,192; for GG it's the centred 160x144 region. */
void smsc_viewport(int *x, int *y, int *w, int *h);

/* Pointer to the most recently rendered frame, as 8-bit palette indices,
 * stride = SMSC_BITMAP_W. */
const uint8_t  *smsc_framebuffer(void);

/* RGB565 palette (256 entries; smsplus replicates 32 entries 8x). */
const uint16_t *smsc_palette_rgb565(void);

/* Set Player 1 controller mask (SMSC_BTN_*). */
void smsc_set_buttons(uint8_t mask);

/* Pull n int16 mono samples produced by the most recent frame. Returns
 * the count actually written. smsplus produces (sndrate / fps + 1)
 * samples per frame; mix L/R streams to mono. */
int  smsc_audio_pull(int16_t *out, int n);

/* Battery-backed cart RAM access. smsplus always allocates a 32 KB
 * scratch buffer per cart, but only some carts actually use it. We
 * expose the pointer unconditionally — caller decides whether to
 * persist based on cart presence. */
uint8_t *smsc_battery_ram (void);
size_t   smsc_battery_size(void);

/* Save-complete signal (see GB / MD equivalents). smsplus's SEGA
 * mapper fires smsc_signal_save_complete() on the SRAM-mapped→
 * unmapped transition. */
void     smsc_signal_save_complete(void);
int      smsc_take_save_pending   (void);

/* Save / load runtime state to a sidecar file (absolute FAT path,
 * e.g. "/Sonic.sta"). Returns 0 on success. Routes through
 * smsplus's system_save_state / system_load_state which use the
 * thumby_state_bridge FatFs shim on the device build. */
int smsc_save_state(const char *path);
int smsc_load_state(const char *path);

/* Tear down. */
void smsc_shutdown(void);
