/*
 * ThumbyNES — Nofrendo glue.
 *
 * Phase 0 stub: wires nofrendo's nes_init / nes_loadfile_mem / nes_emulate
 * to our compact public API. Audio pull and controller injection are
 * still placeholders that will be filled in once we get the host build
 * green and can actually run a frame end-to-end.
 */
#include "nes_core.h"

#include <stdlib.h>
#include <string.h>

#include "nofrendo.h"
#include "nes/nes.h"
#include "nes/apu.h"
#include "nes/input.h"
#include "nes/rom.h"
#include "nes/state.h"

static uint16_t s_palette[256];
static uint8_t  s_buttons;
static int      s_sample_rate;
/* Nofrendo's PPU writes into a host-supplied buffer of size
 * NES_SCREEN_PITCH * NES_SCREEN_HEIGHT bytes (palette indices). */
/* Heap-allocated per-session so we don't pay the 65 KB BSS cost when
 * the NES core is idle (e.g. while the SMS or GB runner is active). */
static uint8_t *s_vidbuf;
#define NES_VIDBUF_BYTES  (NES_SCREEN_PITCH * NES_SCREEN_HEIGHT)

/* Nofrendo blit callback signature: void blit(uint8 *bmp). The bmp
 * pointer is the active PPU framebuffer (with NES_SCREEN_OVERDRAW
 * padding on each side). We just stash it; the host or device layer
 * reads it back via nesc_framebuffer(). */
static const uint8_t *s_last_frame;
static void blit_cb(uint8_t *bmp) { s_last_frame = bmp; }

int nesc_init(int system, int sample_rate)
{
    s_sample_rate = sample_rate;
    s_buttons = 0;
    s_last_frame = NULL;

    if (nofrendo_init(system, sample_rate, false, blit_cb, NULL, NULL) < 0)
        return -1;

    /* Hand the PPU a framebuffer to render into. */
    if (!s_vidbuf) {
        s_vidbuf = (uint8_t *)malloc(NES_VIDBUF_BYTES);
        if (!s_vidbuf) return -2;
    }
    memset(s_vidbuf, 0, NES_VIDBUF_BYTES);
    nes_setvidbuf(s_vidbuf);

    nesc_set_palette(0);
    return 0;
}

void nesc_set_palette(int index)
{
    if (index < 0 || index >= NESC_PALETTE_COUNT) index = 0;
    void *p = nofrendo_buildpalette(index, 16);
    if (p) {
        memcpy(s_palette, p, sizeof(s_palette));
        free(p);
    }
}

const char *nesc_palette_name(int index)
{
    static const char *names[NESC_PALETTE_COUNT] = {
        "NOFRENDO", "COMPOSITE", "NESCLASSC", "NTSC", "PVM", "SMOOTH",
    };
    if (index < 0 || index >= NESC_PALETTE_COUNT) return NULL;
    return names[index];
}

int nesc_load_rom(const uint8_t *data, size_t len)
{
    /* rom_loadmem() takes a non-const pointer because it may patch the
     * iNES header in place for some malformed dumps. We oblige with a
     * cast — on the device the buffer lives in RAM (FatFs read), not in
     * read-only XIP, so the write is safe. */
    rom_t *cart = rom_loadmem((uint8_t *)data, len);
    if (!cart) return -1;
    if (nes_insertcart(cart) < 0) return -2;
    /* nes_reset() (called from nes_insertcart) sets vidbuf=NULL —
     * we have to reattach our buffer after every reset. */
    nes_setvidbuf(s_vidbuf);
    return 0;
}

void nesc_run_frame(void)
{
    /* nes_emulate(true) runs one frame and calls our blit callback. */
    nes_emulate(true);
}

const uint8_t  *nesc_framebuffer(void)        { return s_last_frame; }
int             nesc_framebuffer_pitch(void)  { return NES_SCREEN_PITCH; }
const uint16_t *nesc_palette_rgb565(void)     { return s_palette; }

void nesc_set_buttons(uint8_t mask)
{
    s_buttons = mask;
    input_update(0, mask);
}

int nesc_audio_pull(int16_t *out, int n)
{
    /* nes_emulate() runs apu_emulate() each frame, which fills
     * nes.apu->buffer with samples_per_frame samples. We just hand
     * back whatever we have. The host runner is responsible for
     * matching its audio rate to (sample_rate / 60). */
    nes_t *nes = nes_getptr();
    if (!nes || !nes->apu || !nes->apu->buffer) return 0;
    int avail = nes->apu->samples_per_frame;
    int copy = (n < avail) ? n : avail;
    memcpy(out, nes->apu->buffer, copy * sizeof(int16_t));
    return copy;
}

int nesc_refresh_rate(void)
{
    nes_t *nes = nes_getptr();
    return (nes && nes->refresh_rate) ? nes->refresh_rate : 60;
}

uint8_t *nesc_battery_ram(void)
{
    nes_t *nes = nes_getptr();
    if (!nes || !nes->cart || !nes->cart->battery) return NULL;
    if (nes->cart->prg_ram_banks <= 0) return NULL;
    return nes->cart->prg_ram;
}

size_t nesc_battery_size(void)
{
    nes_t *nes = nes_getptr();
    if (!nes || !nes->cart || !nes->cart->battery) return 0;
    return (size_t)nes->cart->prg_ram_banks * ROM_PRG_BANK_SIZE;
}

int nesc_save_state(const char *path)
{
    if (!path) return -1;
    return state_save(path);
}

int nesc_load_state(const char *path)
{
    if (!path) return -1;
    return state_load(path);
}

void nesc_shutdown(void)
{
    nofrendo_stop();
    nes_shutdown();
    if (s_vidbuf) { free(s_vidbuf); s_vidbuf = NULL; }
}
