/*
 * ThumbyNES — smsplus glue.
 *
 * Mirror of nes_core.c but for the vendored Sega Master System / Game
 * Gear core. The five public operations (init/load/run/buttons/audio)
 * map onto smsplus's `system_*` and `snd.*` API.
 */
#include "sms_core.h"

#include <stdlib.h>
#include <string.h>

#include "shared.h"   /* pulls in system.h, sms.h, sound/sound.h, render.h */
#include "state.h"    /* system_save_state / system_load_state */
#ifdef THUMBY_STATE_BRIDGE
#  include "thumby_state_bridge.h"
#endif

/* The bitmap smsplus renders into. Allocated per-session on the heap
 * so we don't pay the 49 KB BSS cost when the SMS core is idle. */
static uint8_t *s_vidbuf;
static uint16_t s_palette[256];
static int16_t  s_mixbuf[2048];
static int      s_mixcount;
static bool     s_loaded;
/* True if cart.rom was malloc'd by us (and we own it) instead of being
 * an XIP / caller-owned pointer we just borrowed. */
static bool     s_owns_rom;

int smsc_init(int console, int sample_rate)
{
    s_loaded = false;
    s_mixcount = 0;

    /* Allocate the bitmap on first init; reuse across sessions. */
    if (!s_vidbuf) {
        s_vidbuf = (uint8_t *)malloc(SMSC_BITMAP_W * SMSC_BITMAP_H);
        if (!s_vidbuf) return -1;
    }
    memset(s_vidbuf, 0, SMSC_BITMAP_W * SMSC_BITMAP_H);

    /* Reset every option to known defaults, then layer our overrides. */
    system_reset_config();
    option.sndrate  = sample_rate;
    option.console  = console;     /* 0 = auto-detect from ROM header */
    option.overscan = 0;
    option.extra_gg = 0;
    option.tms_pal  = 0;
    return 0;
}

int smsc_load_rom(const uint8_t *data, size_t len)
{
    if (!data || len < 0x4000) return -1;

    /* smsplus reads pages directly out of `data`. If the buffer is
     * already large enough and 16 KB-aligned in length we just hand
     * the pointer through (the device passes us an XIP flash pointer
     * via the picker mmap path — copying that into RAM would waste
     * 256 KB of heap on a Sonic-sized cart). Otherwise we malloc a
     * padded scratch we own.
     *
     * load_rom() stores the pointer in cart.rom; we track ownership
     * via s_owns_rom so smsc_shutdown() only frees what we allocated. */
    s_owns_rom = false;
    uint8_t *rom_ptr;
    int      rom_len = (int)len;
    if ((len & 0x3FFF) == 0) {
        /* Aligned size — borrow caller's pointer (XIP path on device). */
        rom_ptr = (uint8_t *)data;
    } else {
        size_t pad = (len + 0x3FFF) & ~(size_t)0x3FFF;
        uint8_t *buf = malloc(pad);
        if (!buf) return -2;
        memcpy(buf, data, len);
        memset(buf + len, 0xFF, pad - len);
        rom_ptr = buf;
        rom_len = (int)pad;
        s_owns_rom = true;
    }

    if (!load_rom(rom_ptr, rom_len, (int)len)) {
        if (s_owns_rom) { free(rom_ptr); s_owns_rom = false; }
        return -3;
    }

    /* Hand smsplus the framebuffer it should render into. Must happen
     * BEFORE system_poweron() — render_reset() memsets it. */
    bitmap.width  = SMSC_BITMAP_W;
    bitmap.height = SMSC_BITMAP_H;
    bitmap.pitch  = SMSC_BITMAP_W;
    bitmap.data   = s_vidbuf;

    system_poweron();

    /* Force palette regeneration on first frame. */
    memset(s_palette, 0, sizeof(s_palette));
    s_loaded = true;
    return 0;
}

void smsc_run_frame(void)
{
    if (!s_loaded) return;
    system_frame(0);
    /* Refresh palette LUT if smsplus marked it dirty. */
    render_copy_palette(s_palette);

    /* Mix L/R streams down to mono int16 for our consumers. */
    int n = snd.sample_count;
    if (n > (int)(sizeof(s_mixbuf) / sizeof(s_mixbuf[0]))) {
        n = (int)(sizeof(s_mixbuf) / sizeof(s_mixbuf[0]));
    }
    for (int i = 0; i < n; i++) {
        int32_t l = snd.stream[0][i];
        int32_t r = snd.stream[1][i];
        int32_t s = (l + r) >> 1;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        s_mixbuf[i] = (int16_t)s;
    }
    s_mixcount = n;
}

int smsc_refresh_rate(void)
{
    return (sms.display == 1 /* DISPLAY_PAL */) ? 50 : 60;
}

bool smsc_is_gg(void)
{
    return (sms.console & 0x40) != 0; /* HWTYPE_GG */
}

void smsc_viewport(int *x, int *y, int *w, int *h)
{
    if (x) *x = bitmap.viewport.x;
    if (y) *y = bitmap.viewport.y;
    if (w) *w = bitmap.viewport.w;
    if (h) *h = bitmap.viewport.h;
}

const uint8_t  *smsc_framebuffer(void)    { return s_vidbuf; }
const uint16_t *smsc_palette_rgb565(void) { return s_palette; }

void smsc_set_buttons(uint8_t mask)
{
    uint8_t pad = 0;
    uint8_t sys = 0;
    if (mask & SMSC_BTN_UP)      pad |= 0x01; /* INPUT_UP */
    if (mask & SMSC_BTN_DOWN)    pad |= 0x02;
    if (mask & SMSC_BTN_LEFT)    pad |= 0x04;
    if (mask & SMSC_BTN_RIGHT)   pad |= 0x08;
    if (mask & SMSC_BTN_BUTTON1) pad |= 0x10;
    if (mask & SMSC_BTN_BUTTON2) pad |= 0x20;
    if (mask & SMSC_BTN_PAUSE)   sys |= 0x02; /* INPUT_PAUSE (SMS) */
    if (mask & SMSC_BTN_START)   sys |= 0x01; /* INPUT_START (GG) */
    input.pad[0] = pad;
    input.pad[1] = 0;
    input.system = sys;
}

int smsc_audio_pull(int16_t *out, int n)
{
    int copy = (n < s_mixcount) ? n : s_mixcount;
    if (copy > 0) memcpy(out, s_mixbuf, copy * sizeof(int16_t));
    return copy;
}

uint8_t *smsc_battery_ram(void)
{
    return cart.sram;
}

size_t smsc_battery_size(void)
{
    return cart.sram ? 0x8000 : 0;
}

/* Set by smsplus's SEGA mapper when the cart writes the control
 * register transitioning the SRAM-mapped bits 3/4 from "mapped" to
 * "unmapped" — Phantasy Star (SMS), Wonder Boy III, Ys, etc. flip
 * these bits off immediately after writing their save block. The
 * runner consumes via smsc_take_save_pending(). */
static volatile uint8_t s_save_pending;

void smsc_signal_save_complete(void)
{
    s_save_pending = 1;
}

int smsc_take_save_pending(void)
{
    if (!s_save_pending) return 0;
    s_save_pending = 0;
    return 1;
}

int smsc_save_state(const char *path)
{
    if (!path || !s_loaded) return -1;
#ifdef THUMBY_STATE_BRIDGE
    thumby_state_io_t *io = thumby_state_open(path, "wb");
    if (!io) return -2;
    int rc = system_save_state(io);
    thumby_state_close(io);
    return rc;
#else
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    int rc = system_save_state(f);
    fclose(f);
    return rc;
#endif
}

int smsc_load_state(const char *path)
{
    if (!path || !s_loaded) return -1;
#ifdef THUMBY_STATE_BRIDGE
    thumby_state_io_t *io = thumby_state_open(path, "rb");
    if (!io) return -2;
    system_load_state(io);
    thumby_state_close(io);
    return 0;
#else
    FILE *f = fopen(path, "rb");
    if (!f) return -2;
    system_load_state(f);
    fclose(f);
    return 0;
#endif
}

void smsc_shutdown(void)
{
    if (s_loaded) {
        system_shutdown();
        /* Only free cart.rom if we malloc'd it ourselves — XIP-borrowed
         * pointers belong to the caller / live in flash. */
        if (cart.rom && s_owns_rom) free(cart.rom);
        cart.rom = NULL;
        s_owns_rom = false;
        if (cart.sram) { free(cart.sram); cart.sram = NULL; }
        s_loaded = false;
    }
    if (s_vidbuf) { free(s_vidbuf); s_vidbuf = NULL; }
}
