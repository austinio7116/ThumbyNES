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

#include <stdio.h>
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
 * translation unit.
 *
 * Why 44100 Hz internal:
 *   The GB APU's natural rate is ~1 MHz; minigb_apu integrates each
 *   sample over a box-filter-like window which gives only ~13 dB of
 *   stop-band rejection. At 22050 Hz that means square-wave harmonics
 *   above 11 kHz fold back into the audible band as crunchy aliasing
 *   artefacts. Generating internally at 44100 (twice the eventual
 *   output rate) pushes most of the worst aliases above the audible
 *   range, then a properly-designed 2:1 decimating FIR in the runner
 *   cleans up what's left before we publish samples for the device
 *   PWM / host SDL. CPU cost is roughly 2x more APU loop iterations
 *   per frame — ~2% of an RP2350 core, easily affordable. */
#define AUDIO_SAMPLE_RATE              44100
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#include "minigb_apu.h"

/* AUDIO_SAMPLES is computed from a float division so it's not a
 * compile-time constant — use a generous fixed cap for our static
 * stereo buffer. At 44100 Hz / 59.7275 fps that's ~738 stereo
 * frames per video frame; round up to 1024 to absorb fps jitter. */
#define GBC_MAX_FRAME_SAMPLES  1024

/* Output sample rate after 2:1 decimation. Matches the device PWM-IRQ
 * rate and the host's SDL audio open rate. */
#define GBC_OUTPUT_SAMPLE_RATE 22050

/* --- module state --------------------------------------------------- */

/* The peanut_gb emulator state struct is ~49 KB with CGB enabled
 * (32 KB WRAM + 16 KB VRAM + CGB palette RAM + fields). Holding that
 * in BSS would tax every ThumbyNES session, including NES and SMS
 * runs. Allocate on demand in gbc_init and free in gbc_shutdown, the
 * same way s_vidbuf and s_cart_ram are handled. */
static struct gb_s           *s_gb_ptr;
#define s_gb (*s_gb_ptr)

static struct minigb_apu_ctx  s_apu;

static const uint8_t *s_rom;       /* borrowed contiguous pointer */
static size_t         s_rom_len;
static bool           s_loaded;

/* Chained-ROM mode, used when the cart is fragmented on flash and we
 * can't hand peanut_gb a single contiguous pointer. `s_rom` stays
 * NULL in this mode; reads route through the cluster table. Same-
 * cluster reads hit a tiny cache so the extra shift+compare is
 * typically the only overhead per byte. */
static const uint8_t *const *s_rom_clusters;  /* XIP ptr per cluster */
static uint32_t              s_rom_cluster_shift;
static uint32_t              s_rom_cluster_mask;
static uint32_t              s_rom_last_ci;
static const uint8_t        *s_rom_last_ptr;

/* peanut_gb's max addressable cart RAM is 0x8000 (32 KB). Allocated
 * on demand so the GB core costs zero BSS when not running. */
#define GBC_CART_RAM_MAX  0x8000
static uint8_t *s_cart_ram;
static size_t   s_cart_ram_size;

/* Full source framebuffer assembled by the line callback. Heap-
 * allocated per session for the same reason. RGB565 resolved per pixel
 * by lcd_draw_line_cb — blitters just copy words, no palette lookup. */
static uint16_t *s_vidbuf;

/* DMG-only parallel buffer holding the raw shade index (0..3) per
 * pixel. Allocated alongside s_vidbuf for DMG carts and consumed by
 * the runner's palette-aware blend scaler; CGB sessions leave it
 * NULL. ~23 KB (160 × 144 bytes). */
static uint8_t *s_shade_buf;

/* RGB565 palette LUT (4 entries indexed by shade) — used in DMG mode. */
static uint16_t s_palette[4];
static int      s_palette_idx;

/* True when the loaded cart's CGB header flag is set; selects which
 * palette path lcd_draw_line_cb uses. */
static bool     s_cgb_mode;

/* Per-frame stereo scratch (filled by minigb_apu_audio_callback) and
 * the mono mix the runner pulls from. */
static int16_t  s_apu_stereo[GBC_MAX_FRAME_SAMPLES * 2];
static int16_t  s_mixbuf    [GBC_MAX_FRAME_SAMPLES];

/* 2:1 decimating FIR + DC-blocker state, persistent across frames so
 * the filter doesn't get "reset" twice per second and audibly click. */
static int16_t  s_fir_hist[4];      /* last 4 mono samples at 44100 Hz */
static int32_t  s_dc_x_prev;        /* DC blocker x[n-1]               */
static int32_t  s_dc_y_prev;        /* DC blocker y[n-1]               */
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
    /* Hint to the compiler that the chained path is the cold case —
     * the vast majority of sessions run contiguous XIP and we don't
     * want the branch predictor biased the wrong way. */
    if (__builtin_expect(s_rom_clusters != NULL, 0)) {
        uint32_t ci = (uint32_t)(addr >> s_rom_cluster_shift);
        if (ci != s_rom_last_ci) {
            s_rom_last_ci  = ci;
            s_rom_last_ptr = s_rom_clusters[ci];
        }
        return s_rom_last_ptr[addr & s_rom_cluster_mask];
    }
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

/* Set by peanut_gb's MBC layer when the cart writes the RAM-enable
 * register's enabled→disabled transition — i.e. the cart's own
 * "I'm done writing the save" signal. The runner consumes this via
 * gbc_take_save_pending() and flushes cart RAM to its .sav file
 * exactly when the cart is ready, no polling needed. Also covers
 * loaders that disable RAM while reading (Pokemon does this on
 * load-game) — those harmless extra trips through battery_save are
 * absorbed by the CRC gate. */
static volatile uint8_t s_save_pending;

static void gb_cart_ram_disabled_cb(struct gb_s *gb) {
    (void)gb;
    s_save_pending = 1;
}

int gbc_take_save_pending(void) {
    if (!s_save_pending) return 0;
    s_save_pending = 0;
    return 1;
}

static void gb_error_cb(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
    (void)gb; (void)err; (void)addr;
}

static void lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    if (line >= GBC_SCREEN_H) return;
    uint16_t *dst = &s_vidbuf[line * GBC_SCREEN_W];
#if PEANUT_FULL_GBC_SUPPORT
    if (gb->cgb.cgbMode) {
        /* CGB: pixel byte indexes into gb->cgb.fixPalette (BG 0..0x1F,
         * OBJ 0x20..0x3F). The GBC palette RAM stores entries little-
         * endian with R in the low 5 bits and B in bits 10..14. The
         * "swap Red and Blue" step on BCPD/OCPD writes already flips
         * that into standard RGB555 with R high / B low, so we read
         * it in the natural order here:
         *   bits [14..10] = R, [9..5] = G, [4..0] = B
         * Then pack to RGB565 for the GC9107: duplicate G's MSB to
         * widen 5 -> 6 bits so mid-greens don't shift cooler. */
        for (int x = 0; x < GBC_SCREEN_W; x++) {
            uint16_t c15 = gb->cgb.fixPalette[pixels[x]];
            uint16_t r5 = (c15 >> 10) & 0x1F;
            uint16_t g5 = (c15 >>  5) & 0x1F;
            uint16_t b5 =  c15        & 0x1F;
            uint16_t g6 = (g5 << 1) | (g5 >> 4);
            dst[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
        return;
    }
#else
    (void)gb;
#endif
    /* DMG: low 2 bits = shade (0..3). Upper bits are palette source
     * metadata (OBJ0/OBJ1/BG) which we don't use in our palette
     * scheme — we apply a single 4-entry lookup. Shade index also
     * copied into s_shade_buf for the palette-aware scaler. */
    uint8_t *sdst = s_shade_buf ? &s_shade_buf[line * GBC_SCREEN_W] : NULL;
    for (int x = 0; x < GBC_SCREEN_W; x++) {
        uint8_t sh = pixels[x] & 0x03;
        dst[x] = s_palette[sh];
        if (sdst) sdst[x] = sh;
    }
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
    s_cgb_mode = false;
    if (!s_gb_ptr) {
        s_gb_ptr = (struct gb_s *)malloc(sizeof(struct gb_s));
        if (!s_gb_ptr) return -3;
    }
    if (!s_vidbuf) {
        s_vidbuf = (uint16_t *)malloc(GBC_SCREEN_W * GBC_SCREEN_H * sizeof(uint16_t));
        if (!s_vidbuf) return -1;
    }
    if (!s_cart_ram) {
        s_cart_ram = (uint8_t *)malloc(GBC_CART_RAM_MAX);
        if (!s_cart_ram) return -2;
    }
    memset(s_gb_ptr, 0, sizeof(struct gb_s));
    memset(s_vidbuf, 0, GBC_SCREEN_W * GBC_SCREEN_H * sizeof(uint16_t));
    memset(s_cart_ram, 0, GBC_CART_RAM_MAX);
    /* Anti-alias FIR + DC-blocker state belongs to the audio
     * pipeline, not to a particular cart — but resetting here
     * avoids a brief click at boot when we'd otherwise convolve
     * the new APU output with the previous cart's tail samples. */
    memset(s_fir_hist, 0, sizeof(s_fir_hist));
    s_dc_x_prev = 0;
    s_dc_y_prev = 0;
    gbc_set_palette(GBC_PALETTE_GREEN);
    return 0;
}

/* Shared post-configure-rom tail: peanut_gb init, save-ram sizing,
 * LCD + APU hookup. Called by both gbc_load_rom (contiguous) and
 * gbc_load_rom_chain (chained) after the s_rom / s_rom_clusters
 * pointers are set. */
static int gbc_finish_load(void) {
    enum gb_init_error_e err = gb_init(&s_gb,
                                        gb_rom_read_cb,
                                        gb_cart_ram_read_cb,
                                        gb_cart_ram_write_cb,
                                        gb_error_cb,
                                        NULL);
    if (err != GB_INIT_NO_ERROR) return -(int)err - 1;
    /* Save-complete callback. peanut_gb fires this when the cart
     * writes the MBC RAM-enable register's enabled→disabled
     * transition — Pokemon's "save just finished" signal. */
    s_gb.gb_cart_ram_disabled = gb_cart_ram_disabled_cb;
    s_save_pending = 0;

    size_t want = (size_t)gb_get_save_size(&s_gb);
    if (want > GBC_CART_RAM_MAX) want = GBC_CART_RAM_MAX;
    s_cart_ram_size = want;

    gb_init_lcd(&s_gb, lcd_draw_line_cb);
    minigb_apu_audio_init(&s_apu);
#if PEANUT_FULL_GBC_SUPPORT
    s_cgb_mode = (s_gb.cgb.cgbMode != 0);
#endif
    /* DMG carts get a shade-index buffer so the runner's scaler can
     * blend in palette-index space. CGB has per-pixel CGB palette
     * resolution, there is no meaningful global "shade" to index. */
    if (!s_cgb_mode) {
        if (!s_shade_buf) {
            s_shade_buf = (uint8_t *)malloc(GBC_SCREEN_W * GBC_SCREEN_H);
            /* Failure is non-fatal — runner falls back to RGB565
             * blend on this cart (with the mild teal shift). */
        }
        if (s_shade_buf) memset(s_shade_buf, 0, GBC_SCREEN_W * GBC_SCREEN_H);
    } else if (s_shade_buf) {
        /* Previous session was DMG; reclaim the buffer so CGB carts
         * don't carry the extra 23 KB. */
        free(s_shade_buf);
        s_shade_buf = NULL;
    }
    s_loaded = true;
    return 0;
}

int gbc_load_rom(const uint8_t *data, size_t len) {
    if (!data || len < 0x150) return -1;
    s_rom          = data;
    s_rom_len      = len;
    s_rom_clusters = NULL;   /* contiguous fast path */
    return gbc_finish_load();
}

int gbc_load_rom_chain(const uint8_t * const *cluster_ptrs,
                        uint32_t cluster_shift,
                        uint32_t cluster_mask,
                        size_t   len) {
    if (!cluster_ptrs || len < 0x150) return -1;
    s_rom                 = NULL;
    s_rom_len             = len;
    s_rom_clusters        = cluster_ptrs;
    s_rom_cluster_shift   = cluster_shift;
    s_rom_cluster_mask    = cluster_mask;
    s_rom_last_ci         = 0xFFFFFFFFu;   /* force first lookup */
    s_rom_last_ptr        = NULL;
    return gbc_finish_load();
}

void gbc_run_frame(void) {
    if (!s_loaded) return;
    gb_run_frame(&s_gb);

    /* Pull one frame's worth of audio from minigb_apu (44100 Hz
     * stereo). We mix to mono, low-pass-filter with a 5-tap binomial
     * FIR ([1,4,6,4,1]/16, -3 dB at fs/4 = output Nyquist), decimate
     * 2:1 to 22050 Hz, and run the result through a one-pole DC-
     * blocker before clipping into the int16 output buffer. */
    minigb_apu_audio_callback(&s_apu, s_apu_stereo);
    int n_in = (int)AUDIO_SAMPLES;
    if (n_in > GBC_MAX_FRAME_SAMPLES) n_in = GBC_MAX_FRAME_SAMPLES;

    /* Local FIR/DC state copies — keep registers warm in the inner
     * loop instead of bouncing through the static globals. */
    int32_t h0 = s_fir_hist[0];
    int32_t h1 = s_fir_hist[1];
    int32_t h2 = s_fir_hist[2];
    int32_t h3 = s_fir_hist[3];
    int32_t dc_x = s_dc_x_prev;
    int32_t dc_y = s_dc_y_prev;

    int n_out = 0;
    for (int i = 0; i < n_in; i++) {
        int32_t l = s_apu_stereo[i * 2];
        int32_t r = s_apu_stereo[i * 2 + 1];
        int32_t mono = (l + r) >> 1;

        /* 5-tap FIR centered on h2 (= input sample 2 frames ago).
         * Window is [h0, h1, h2, h3, mono] → x[i-4..i]. The output
         * corresponds to input x[i-2]; group delay is 2 input
         * samples (~45 us at 44100 Hz, inaudible). */
        int32_t y = h0 + 4 * h1 + 6 * h2 + 4 * h3 + mono;
        y >>= 4;   /* /16 */

        /* Slide history: drop h0, append mono. */
        h0 = h1;
        h1 = h2;
        h2 = h3;
        h3 = mono;

        /* Decimate 2:1 — keep only every other filtered sample. */
        if ((i & 1) == 0) continue;

        /* DC blocker: y[n] = x[n] - x[n-1] + a * y[n-1], a ≈ 0.998
         * (a = 32700/32768). Cutoff ≈ 7 Hz at 22050 Hz — removes the
         * envelope-change DC pulses that minigb_apu doesn't model
         * (real GB hardware has a ~120 Hz HPF on each channel). */
        int32_t x_now = y;
        y = x_now - dc_x + ((dc_y * 32700) >> 15);
        dc_x = x_now;
        dc_y = y;

        if (y >  32767) y =  32767;
        if (y < -32768) y = -32768;
        s_mixbuf[n_out++] = (int16_t)y;
    }

    /* Persist FIR/DC state for next frame. */
    s_fir_hist[0] = (int16_t)h0;
    s_fir_hist[1] = (int16_t)h1;
    s_fir_hist[2] = (int16_t)h2;
    s_fir_hist[3] = (int16_t)h3;
    s_dc_x_prev   = dc_x;
    s_dc_y_prev   = dc_y;

    s_mixcount = n_out;
}

int gbc_refresh_rate(void) {
    return 60;
}

const uint16_t *gbc_framebuffer(void)    { return s_vidbuf; }
const uint16_t *gbc_palette_rgb565(void) { return s_palette; }
bool            gbc_is_cgb_cart(void)    { return s_cgb_mode; }
const uint8_t  *gbc_shade_buffer(void)   { return s_shade_buf; }

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

#include <time.h>

void gbc_set_rtc(const struct tm *t) {
    if (!s_loaded || !t) return;
    /* peanut_gb takes a non-const struct tm * (legacy API); copy off
     * the const since the lib doesn't actually mutate the input. */
    struct tm copy = *t;
    gb_set_rtc(&s_gb, &copy);
}

void gbc_tick_rtc(void) {
    if (!s_loaded) return;
    /* peanut_gb's gb_tick_rtc handles the "halt RTC" bit (bit 6 of
     * the day-high byte) internally — no-op if the cart has stopped
     * the clock via a write to the day register. */
    gb_tick_rtc(&s_gb);
}

void gbc_peek_cart_rtc(uint8_t out[5]) {
    if (!out) return;
    if (!s_loaded) { for (int i = 0; i < 5; i++) out[i] = 0xFF; return; }
    for (int i = 0; i < 5; i++) out[i] = s_gb.cart_rtc[i];
}

void gbc_poke_cart_rtc(const uint8_t in[5]) {
    if (!in || !s_loaded) return;
    for (int i = 0; i < 5; i++) s_gb.cart_rtc[i] = in[i];
}

/* Save state magic + version.
 * V1: hdr[3] = magic, version, payload_size. payload = gb_s + apu.
 * V2: hdr[3] = magic, version, payload_size. payload = gb_s + apu +
 *     int64 wall_clock_unix_secs_at_save (8 bytes). On load, the
 *     runner can advance cart_rtc by (now - saved) so Pokemon's day/
 *     night and berry growth track real wall clock across the time
 *     a state was sitting on disk. V1 still loadable (we read 0 for
 *     wall-clock and skip the advance). */
#define GBC_STATE_MAGIC     0x47424353u   /* 'GBCS' */
#define GBC_STATE_VERSION_1 1u
#define GBC_STATE_VERSION_2 2u
#define GBC_STATE_VERSION   GBC_STATE_VERSION_2

#ifdef THUMBY_STATE_BRIDGE
#  include "thumby_state_bridge.h"
#endif

int gbc_save_state(const char *path, int64_t wall_clock_unix_secs) {
    if (!s_loaded || !path) return -1;
    uint32_t payload_sz = (uint32_t)(sizeof(s_gb) + sizeof(s_apu) + sizeof(int64_t));
    uint32_t hdr[3] = {
        GBC_STATE_MAGIC,
        GBC_STATE_VERSION,
        payload_sz,
    };
#ifdef THUMBY_STATE_BRIDGE
    thumby_state_io_t *io = thumby_state_open(path, "wb");
    if (!io) return -2;
    if (thumby_state_write(hdr, sizeof(hdr), 1, io) != 1
     || thumby_state_write(&s_gb,  sizeof(s_gb),  1, io) != 1
     || thumby_state_write(&s_apu, sizeof(s_apu), 1, io) != 1
     || thumby_state_write(&wall_clock_unix_secs, sizeof(wall_clock_unix_secs), 1, io) != 1) {
        thumby_state_close(io);
        return -3;
    }
    thumby_state_close(io);
    return 0;
#else
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    int ok = fwrite(hdr, sizeof(hdr), 1, f) == 1
          && fwrite(&s_gb,  sizeof(s_gb),  1, f) == 1
          && fwrite(&s_apu, sizeof(s_apu), 1, f) == 1
          && fwrite(&wall_clock_unix_secs, sizeof(wall_clock_unix_secs), 1, f) == 1;
    fclose(f);
    return ok ? 0 : -3;
#endif
}

int gbc_load_state(const char *path, int64_t *out_saved_wall_unix_secs) {
    if (!s_loaded || !path) return -1;
    uint32_t hdr[3];
    /* Pre-zero the out pointer so a V1 file (which lacks the wall-
     * clock tail) leaves the runner with 0 — runner treats that as
     * "no wall-delta info, advance nothing". */
    if (out_saved_wall_unix_secs) *out_saved_wall_unix_secs = 0;

    /* Accept V1 (no wall-clock tail) or V2 (with wall-clock tail).
     * Payload size is what tells V1/V2 apart at parse time. */
    uint32_t v1_payload = (uint32_t)(sizeof(s_gb) + sizeof(s_apu));
    uint32_t v2_payload = v1_payload + (uint32_t)sizeof(int64_t);
#ifdef THUMBY_STATE_BRIDGE
    thumby_state_io_t *io = thumby_state_open(path, "rb");
    if (!io) return -2;
    if (thumby_state_read(hdr, sizeof(hdr), 1, io) != 1) { thumby_state_close(io); return -3; }
    if (hdr[0] != GBC_STATE_MAGIC) { thumby_state_close(io); return -4; }
    bool is_v2 = false;
    if (hdr[1] == GBC_STATE_VERSION_1 && hdr[2] == v1_payload) {
        is_v2 = false;
    } else if (hdr[1] == GBC_STATE_VERSION_2 && hdr[2] == v2_payload) {
        is_v2 = true;
    } else {
        thumby_state_close(io); return -4;
    }
    if (thumby_state_read(&s_gb,  sizeof(s_gb),  1, io) != 1
     || thumby_state_read(&s_apu, sizeof(s_apu), 1, io) != 1) {
        thumby_state_close(io); return -5;
    }
    if (is_v2) {
        int64_t wall = 0;
        if (thumby_state_read(&wall, sizeof(wall), 1, io) != 1) {
            thumby_state_close(io); return -5;
        }
        if (out_saved_wall_unix_secs) *out_saved_wall_unix_secs = wall;
    }
    thumby_state_close(io);
#else
    FILE *f = fopen(path, "rb");
    if (!f) return -2;
    if (fread(hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return -3; }
    if (hdr[0] != GBC_STATE_MAGIC) { fclose(f); return -4; }
    bool is_v2 = false;
    if (hdr[1] == GBC_STATE_VERSION_1 && hdr[2] == v1_payload) {
        is_v2 = false;
    } else if (hdr[1] == GBC_STATE_VERSION_2 && hdr[2] == v2_payload) {
        is_v2 = true;
    } else {
        fclose(f); return -4;
    }
    if (fread(&s_gb,  sizeof(s_gb),  1, f) != 1
     || fread(&s_apu, sizeof(s_apu), 1, f) != 1) {
        fclose(f); return -5;
    }
    if (is_v2) {
        int64_t wall = 0;
        if (fread(&wall, sizeof(wall), 1, f) != 1) { fclose(f); return -5; }
        if (out_saved_wall_unix_secs) *out_saved_wall_unix_secs = wall;
    }
    fclose(f);
#endif
    /* Re-attach function pointers — they were serialized but the
     * deserialized values point into the old wrapper's text section
     * (or in our case, the same wrapper, but the principle is the
     * same — clobber them with known-good pointers anyway). */
    s_gb.gb_rom_read           = gb_rom_read_cb;
    s_gb.gb_cart_ram_read      = gb_cart_ram_read_cb;
    s_gb.gb_cart_ram_write     = gb_cart_ram_write_cb;
    s_gb.gb_cart_ram_disabled  = gb_cart_ram_disabled_cb;
    s_gb.gb_error              = gb_error_cb;
    s_gb.display.lcd_draw_line = lcd_draw_line_cb;
    s_gb.gb_serial_tx = NULL;
    s_gb.gb_serial_rx = NULL;
    s_gb.gb_bootrom_read = NULL;
    s_gb.direct.priv = NULL;
    return 0;
}

void gbc_shutdown(void) {
    s_loaded        = false;
    s_rom           = NULL;
    s_rom_len       = 0;
    s_cart_ram_size = 0;
    /* The chain array itself is caller-owned (the runner allocates it
     * via nes_picker_mmap_rom_chain and frees it after gbc_shutdown
     * returns). Just clear our pointer so a subsequent gbc_load_rom
     * goes back to the contiguous fast path. */
    s_rom_clusters = NULL;
    s_rom_last_ci  = 0xFFFFFFFFu;
    s_rom_last_ptr = NULL;
    if (s_vidbuf)    { free(s_vidbuf);    s_vidbuf    = NULL; }
    if (s_shade_buf) { free(s_shade_buf); s_shade_buf = NULL; }
    if (s_cart_ram)  { free(s_cart_ram);  s_cart_ram  = NULL; }
    if (s_gb_ptr)    { free(s_gb_ptr);    s_gb_ptr    = NULL; }
}
