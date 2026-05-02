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

/* Returns the cart's PRG-RAM regardless of the iNES header's battery
 * flag. A surprising number of widely-circulated NES dumps (older
 * Zelda 1 dumps especially, but also some Final Fantasy / Crystalis
 * dumps) have the battery bit cleared in byte 6 of the iNES header
 * even though the original cart shipped with battery RAM. The
 * stricter check we used to do here meant those dumps silently
 * never wrote .sav files. We now rely on the dirty-driven save path
 * (`nesc_take_sram_dirty()` + battery_save's CRC gate): a cart that
 * never writes SRAM keeps its CRC equal to the all-zero seed and
 * never produces a .sav, so dropping the flag costs nothing on
 * carts that genuinely don't have battery; carts that do write
 * SRAM finally get their saves persisted regardless of header. */
uint8_t *nesc_battery_ram(void)
{
    nes_t *nes = nes_getptr();
    if (!nes || !nes->cart) return NULL;
    if (nes->cart->prg_ram_banks <= 0) return NULL;
    return nes->cart->prg_ram;
}

size_t nesc_battery_size(void)
{
    nes_t *nes = nes_getptr();
    if (!nes || !nes->cart) return 0;
    if (nes->cart->prg_ram_banks <= 0) return 0;
    return (size_t)nes->cart->prg_ram_banks * ROM_PRG_BANK_SIZE;
}

/* Set by mappers (MMC1, MMC3 — see vendor patches) when the cart
 * writes the WRAM-disable transition: MMC1's register 3 bit 4
 * going 0→1, MMC3's A001 bit 7 going 1→0. Real NES carts that use
 * battery (Final Fantasy / Crystalis on MMC1B+, Kirby's Adventure
 * on MMC3) flip this bit immediately after writing the save block,
 * mirroring the GB MBC RAM-disable pattern. */
static volatile uint8_t s_save_pending;

void nesc_signal_save_complete(void)
{
    s_save_pending = 1;
}

int nesc_take_save_pending(void)
{
    if (!s_save_pending) return 0;
    s_save_pending = 0;
    return 1;
}

/* Set by nofrendo's mem_putbyte on every CPU write to $6000-$7FFF
 * (cart PRG-RAM range). Catches carts that don't toggle the
 * WRAM-disable register — notably Zelda 1, an early MMC1 (likely
 * MMC1A) board where the disable bit was either not present or
 * not used by the cart code. The runner debounces on this: when
 * 500 ms have elapsed since the last SRAM write, fire battery_save.
 * Driven by the actual writes rather than CRC scanning so the
 * detection is free per-frame. */
extern volatile int nesc_sram_dirty;

int nesc_take_sram_dirty(void)
{
    if (!nesc_sram_dirty) return 0;
    nesc_sram_dirty = 0;
    return 1;
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
