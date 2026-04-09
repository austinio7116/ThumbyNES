/*
 * ThumbyNES — peanut_gb / minigb_apu glue.
 *
 * Mirror of nes_core.c and sms_core.c. Wires the vendored single-
 * header peanut_gb DMG core through to our compact public API and
 * mixes the stereo APU output down to mono for the existing PWM /
 * SDL audio paths.
 *
 * Notes
 * -----
 * peanut_gb is a strict scanline emulator — it calls our
 * `lcd_draw_line_cb` 144 times per frame with one row of palette
 * indices. We assemble the full 160×144 frame into a static buffer
 * (`s_vidbuf`) so the runner has a flat framebuffer to scale.
 *
 * The cart ROM is borrowed: we never copy it. On the device the
 * picker hands us an XIP flash pointer; on the host we hold the
 * malloc'd buffer ourselves and free it on shutdown.
 *
 * Audio is produced one frame at a time by minigb_apu's callback,
 * which fills 2 × ~369 = ~738 stereo samples. We average L+R into
 * `s_mixbuf` and the runner pulls from there exactly the way the
 * SMS path works.
 */
#include "gb_core.h"

#include <stdlib.h>
#include <string.h>

/* peanut_gb's ENABLE_SOUND path calls bare `audio_read`/`audio_write`
 * symbols without prior declarations, so it implicitly types them.
 * Forward-declare them with the exact signatures we'll define below
 * to avoid the implicit-declaration mismatch. */
#include <stdint.h>
void    audio_write(uint16_t addr, uint8_t val);
uint8_t audio_read (uint16_t addr);

/* Pull in the full peanut_gb implementation in this translation unit.
 * Other files that need the typedefs only set PEANUT_GB_HEADER_ONLY. */
#define ENABLE_LCD     1
#define ENABLE_SOUND   1
#include "peanut_gb.h"

/* minigb_apu requires the format + sample-rate defines before its
 * header is included. The .c shim sets the same values for the impl
 * translation unit. */
#define AUDIO_SAMPLE_RATE              22050
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#include "minigb_apu.h"

/* AUDIO_SAMPLES is computed from a float division so it's not a
 * compile-time constant — use a generous fixed cap for our static
 * buffers (22050 / 59.7275 ≈ 369 stereo samples per frame). */
#define GBC_MAX_FRAME_SAMPLES  512

/* --- module state --------------------------------------------------- */

static struct gb_s            s_gb;
static struct minigb_apu_ctx  s_apu;

static const uint8_t *s_rom;       /* borrowed pointer */
static size_t         s_rom_len;
static bool           s_loaded;

/* peanut_gb's max addressable cart RAM is 0x8000 (32 KB). Allocated
 * on demand so the GB core costs zero BSS when not running. */
#define GBC_CART_RAM_MAX  0x8000
static uint8_t *s_cart_ram;
static size_t   s_cart_ram_size;

/* Full source framebuffer assembled by the line callback. Heap-
 * allocated per session for the same reason. */
static uint8_t *s_vidbuf;

/* RGB565 palette LUT (4 entries indexed by shade). */
static uint16_t s_palette[4];
static int      s_palette_idx;

/* Per-frame stereo scratch (filled by minigb_apu_audio_callback) and
 * the mono mix the runner pulls from. */
static int16_t  s_apu_stereo[GBC_MAX_FRAME_SAMPLES * 2];
static int16_t  s_mixbuf    [GBC_MAX_FRAME_SAMPLES];
static int      s_mixcount;

/* Built-in palettes (RGB565, shade 0 = lightest, shade 3 = darkest).
 * Lifted from the engine's gb_emu_core for visual parity. */
static const uint16_t s_palettes[GBC_PALETTE_COUNT][4] = {
    /* Classic Game Boy LCD green */
    { 0x9DE1, 0x8D60, 0x3300, 0x0A00 },
    /* Grayscale */
    { 0xFFFF, 0xAD55, 0x52AA, 0x0000 },
    /* Game Boy Pocket */
    { 0xE79C, 0xB596, 0x6B4D, 0x2104 },
    /* Cream */
    { 0xFFF5, 0xDECA, 0x9C60, 0x4200 },
    /* Blue */
    { 0xDF9F, 0x5D5F, 0x2A5E, 0x0010 },
    /* Red */
    { 0xFFFF, 0xFBCA, 0xC180, 0x6000 },
};

static const char * const s_palette_names[GBC_PALETTE_COUNT] = {
    "GREEN", "GREY", "POCKET", "CREAM", "BLUE", "RED",
};

/* --- peanut_gb callbacks ------------------------------------------- */

static uint8_t gb_rom_read_cb(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb;
    if (addr >= s_rom_len) return 0xFF;
    return s_rom[addr];
}

static uint8_t gb_cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb;
    if (addr >= s_cart_ram_size) return 0xFF;
    return s_cart_ram[addr];
}

static void gb_cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
    (void)gb;
    if (addr < s_cart_ram_size) s_cart_ram[addr] = val;
}

static void gb_error_cb(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
    (void)gb; (void)err; (void)addr;
}

static void lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    (void)gb;
    if (line >= GBC_SCREEN_H) return;
    /* peanut_gb encodes the shade in the low 2 bits and a palette
     * select in higher bits — we only care about the shade. */
    uint8_t *dst = &s_vidbuf[line * GBC_SCREEN_W];
    for (int x = 0; x < GBC_SCREEN_W; x++) dst[x] = pixels[x] & 0x03;
}

/* peanut_gb expects these as global symbols when ENABLE_SOUND=1. */
void audio_write(uint16_t addr, uint8_t val) {
    minigb_apu_audio_write(&s_apu, addr, val);
}

uint8_t audio_read(uint16_t addr) {
    return minigb_apu_audio_read(&s_apu, addr);
}

/* --- public API ---------------------------------------------------- */

int gbc_init(int sample_rate) {
    (void)sample_rate;   /* fixed in vendored minigb_apu wrapper */
    s_loaded   = false;
    s_mixcount = 0;
    if (!s_vidbuf) {
        s_vidbuf = (uint8_t *)malloc(GBC_SCREEN_W * GBC_SCREEN_H);
        if (!s_vidbuf) return -1;
    }
    if (!s_cart_ram) {
        s_cart_ram = (uint8_t *)malloc(GBC_CART_RAM_MAX);
        if (!s_cart_ram) return -2;
    }
    memset(s_vidbuf, 0, GBC_SCREEN_W * GBC_SCREEN_H);
    memset(s_cart_ram, 0, GBC_CART_RAM_MAX);
    gbc_set_palette(GBC_PALETTE_GREEN);
    return 0;
}

int gbc_load_rom(const uint8_t *data, size_t len) {
    if (!data || len < 0x150) return -1;
    s_rom     = data;
    s_rom_len = len;

    enum gb_init_error_e err = gb_init(&s_gb,
                                        gb_rom_read_cb,
                                        gb_cart_ram_read_cb,
                                        gb_cart_ram_write_cb,
                                        gb_error_cb,
                                        NULL);
    if (err != GB_INIT_NO_ERROR) return -(int)err - 1;

    /* Ask peanut_gb how much cart RAM the loaded MBC actually uses
     * and clamp to our scratch buffer. */
    size_t want = 0;
    gb_get_save_size_s(&s_gb, &want);
    if (want > GBC_CART_RAM_MAX) want = GBC_CART_RAM_MAX;
    s_cart_ram_size = want;

    gb_init_lcd(&s_gb, lcd_draw_line_cb);
    minigb_apu_audio_init(&s_apu);
    s_loaded = true;
    return 0;
}

void gbc_run_frame(void) {
    if (!s_loaded) return;
    gb_run_frame(&s_gb);

    /* Pull one frame's worth of audio from minigb_apu and mix to
     * mono with hard clipping. */
    minigb_apu_audio_callback(&s_apu, s_apu_stereo);
    int n = (int)AUDIO_SAMPLES;   /* runtime float -> int, ~369 */
    if (n > GBC_MAX_FRAME_SAMPLES) n = GBC_MAX_FRAME_SAMPLES;
    for (int i = 0; i < n; i++) {
        int32_t l = s_apu_stereo[i * 2];
        int32_t r = s_apu_stereo[i * 2 + 1];
        int32_t s = (l + r) >> 1;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        s_mixbuf[i] = (int16_t)s;
    }
    s_mixcount = n;
}

int gbc_refresh_rate(void) {
    return 60;
}

const uint8_t  *gbc_framebuffer(void)    { return s_vidbuf; }
const uint16_t *gbc_palette_rgb565(void) { return s_palette; }

void gbc_set_palette(int index) {
    if (index < 0 || index >= GBC_PALETTE_COUNT) index = 0;
    s_palette_idx = index;
    memcpy(s_palette, s_palettes[index], sizeof(s_palette));
}

const char *gbc_palette_name(int index) {
    if (index < 0 || index >= GBC_PALETTE_COUNT) return NULL;
    return s_palette_names[index];
}

void gbc_set_buttons(uint8_t mask) {
    if (!s_loaded) return;
    /* peanut_gb encodes "pressed = bit clear" in the joypad register,
     * so we invert the user's "pressed = bit set" mask. */
    s_gb.direct.joypad = (uint8_t)~mask;
}

int gbc_audio_pull(int16_t *out, int n) {
    int copy = (n < s_mixcount) ? n : s_mixcount;
    if (copy > 0) memcpy(out, s_mixbuf, copy * sizeof(int16_t));
    return copy;
}

uint8_t *gbc_battery_ram(void) {
    return (s_cart_ram_size > 0) ? s_cart_ram : NULL;
}

size_t gbc_battery_size(void) {
    return s_cart_ram_size;
}

void gbc_shutdown(void) {
    s_loaded        = false;
    s_rom           = NULL;
    s_rom_len       = 0;
    s_cart_ram_size = 0;
    if (s_vidbuf)   { free(s_vidbuf);   s_vidbuf   = NULL; }
    if (s_cart_ram) { free(s_cart_ram); s_cart_ram = NULL; }
}
