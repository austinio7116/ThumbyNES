/*
 * ThumbyNES — device-side Game Boy runner.
 *
 * Mirrors sms_run.c shape but drives peanut_gb instead of smsplus.
 * Reuses every hardware driver as-is (LCD, audio PWM, buttons, FAT
 * battery sidecars, idle-sleep). The only GB-specific bits are the
 * scaler set and the controller bit layout.
 *
 * The Game Boy is 160x144 native; the Thumby Color LCD is 128x128.
 * Two scale modes:
 *
 *   FIT  — asymmetric 5:4 × 9:8 scaling, fills the whole screen
 *          (same trick we use for Game Gear). 160 → 128 horizontally
 *          and 144 → 128 vertically. The BLEND toggle selects between
 *          pure nearest (sharp, but drops 1 in 5 source columns) and
 *          a coverage-weighted blend (palette-aware on DMG; packed
 *          RGB565 lerp on CGB). Default: BLEND on.
 *   CROP — 1:1 native 128x128 viewport into the 160x144 frame,
 *          pannable with MENU + D-pad while the cart keeps running.
 *          The full picture has 32 px horizontal slack and 16 px
 *          vertical.
 */
#include "gb_run.h"
#include "gb_core.h"
#include "nes_picker.h"
#include "nes_lcd_gc9107.h"
#include "nes_audio_pwm.h"
#include "nes_buttons.h"
#include "nes_font.h"
#include "nes_thumb.h"
#include "nes_menu.h"
#include "nes_flash_disk.h"

#ifdef THUMBYONE_SLOT_MODE
#  include "thumbyone_settings.h"
#  include "thumbyone_backlight.h"
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "ff.h"

/* Pin map mirrors nes_run.c. */
#define BTN_LEFT_GP   0
#define BTN_UP_GP     1
#define BTN_RIGHT_GP  2
#define BTN_DOWN_GP   3
#define BTN_LB_GP     6
#define BTN_A_GP     21
#define BTN_RB_GP    22
#define BTN_B_GP     25
#define BTN_MENU_GP  26

static void make_sidecar_path(char *out, size_t outsz,
                               const char *rom_name, const char *ext) {
    char base[64];
    strncpy(base, rom_name, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    snprintf(out, outsz, ROMS_DIR_SLASH "%s%s", base, ext);
}

static void battery_load(const char *rom_name) {
    uint8_t *ram = gbc_battery_ram();
    size_t   sz  = gbc_battery_size();
    if (!ram || sz == 0) return;
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".sav");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return;
    UINT br = 0;
    f_read(&f, ram, (UINT)sz, &br);
    f_close(&f);
}

static void battery_save(const char *rom_name) {
    uint8_t *ram = gbc_battery_ram();
    size_t   sz  = gbc_battery_size();
    if (!ram || sz == 0) return;
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".sav");
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT bw = 0;
    f_write(&f, ram, (UINT)sz, &bw);
    f_close(&f);
}

/* GB joypad bitmask from Thumby buttons. */
static uint8_t read_gb_buttons(void) {
    uint8_t m = 0;
    if (!gpio_get(BTN_A_GP))     m |= GBC_BTN_A;
    if (!gpio_get(BTN_B_GP))     m |= GBC_BTN_B;
    if (!gpio_get(BTN_LB_GP))    m |= GBC_BTN_SELECT;
    if (!gpio_get(BTN_RB_GP))    m |= GBC_BTN_START;
    if (!gpio_get(BTN_UP_GP))    m |= GBC_BTN_UP;
    if (!gpio_get(BTN_DOWN_GP))  m |= GBC_BTN_DOWN;
    if (!gpio_get(BTN_LEFT_GP))  m |= GBC_BTN_LEFT;
    if (!gpio_get(BTN_RIGHT_GP)) m |= GBC_BTN_RIGHT;
    return m;
}

/* --- scalers ------------------------------------------------------- */

/* --- FIT scalers ---------------------------------------------------
 *
 * Three FIT variants, selected by the in-game BLEND toggle and the
 * DMG-vs-CGB cart flag:
 *
 *   blit_fit_gb_nearest     — asymmetric 5:4 × 9:8 nearest; pixel-
 *                             perfect but drops every 5th source
 *                             column and every 9th row. Used when
 *                             BLEND is OFF.
 *   blit_fit_gb_cgb_blend   — area-weighted coverage blend in RGB565
 *                             via the packed-lerp trick. Used for
 *                             CGB carts when BLEND is ON.
 *   blit_fit_gb_dmg_blend   — palette-index-space blend for DMG
 *                             carts: resolve 4 shade indices from
 *                             the core's shade buffer, blend in
 *                             0..3 fixed-point, then interpolate
 *                             between the two bracketing palette
 *                             entries in RGB565. Preserves the
 *                             chosen DMG palette's hue because the
 *                             gradient is walked monotonically
 *                             rather than linearly-blended in
 *                             channel space (the classic Nintendo
 *                             green's shade 0 and shade 2 differ
 *                             enough in their blue component that
 *                             a raw RGB565 blend reads as
 *                             teal-shifted).
 *
 * Coverage weights: horizontal pattern repeats every 4 output pixels
 * over 5 source pixels → {0.80/0.20, 0.60/0.40, 0.40/0.60, 0.20/0.80};
 * vertical repeats every 8 output pixels over 9 source → {0.111, 0.222,
 * …, 0.889}. Both expressed in /32 for a shift-by-5 after the lerp.
 */

/* Nearest, kept for users who toggle BLEND off (sharper pixel art
 * at the cost of dropped strokes). */
static void blit_fit_gb_nearest(uint16_t *fb, const uint16_t *src) {
    for (int dy = 0; dy < 128; dy++) {
        int sy = (dy * 9) / 8;
        const uint16_t *srow = src + sy * GBC_SCREEN_W;
        uint16_t       *drow = fb + dy * 128;
        for (int dx = 0; dx < 128; dx++) {
            int sx = (dx * 5) / 4;
            drow[dx] = srow[sx];
        }
    }
}

/* Packed-RGB565 linear interpolate — ONE multiply per lerp (all three
 * channels in parallel). Expand `p | (p << 16)` masked with
 * 0x07E0F81F places R at bits 11..15, B at 0..4 (low 16) and G at
 * 21..26 (high 16) with gap bits between. `A + ((B - A) * w >> 5) &
 * mask` survives unsigned underflow because the mask drops borrow
 * bits that propagate into those gaps during the subtract. w is in
 * /32 (5-bit fraction). */
static inline uint32_t exp565(uint16_t p) {
    uint32_t x = p;
    return (x | (x << 16)) & 0x07E0F81Fu;
}
static inline uint32_t lerp565p(uint32_t A, uint32_t B, uint32_t w) {
    return (A + (((B - A) * w) >> 5)) & 0x07E0F81Fu;
}
static inline uint16_t pak565(uint32_t c) {
    return (uint16_t)((c | (c >> 16)) & 0xFFFFu);
}

/* Shared coverage-weight tables — 4 × horizontal (dx & 3) and
 * 8 × vertical (dy & 7), weight on the "far" source pixel in /32. */
static const uint8_t gb_fit_hw[4] = { 6, 13, 19, 26 };
static const uint8_t gb_fit_vw[8] = { 4, 7, 11, 14, 18, 21, 25, 28 };

/* CGB — RGB565 coverage blend via packed lerp.
 * 3 multiplies per output pixel (top horizontal, bottom horizontal,
 * vertical). Runs from SRAM. */
static void __not_in_flash_func(blit_fit_gb_cgb_blend)(uint16_t *fb,
                                                        const uint16_t *src) {
    for (int dy = 0; dy < 128; dy++) {
        int sy = (dy * 9) / 8;
        uint32_t wy = gb_fit_vw[dy & 7];
        const uint16_t *r0 = src + sy * GBC_SCREEN_W;
        const uint16_t *r1 = src + (sy + 1) * GBC_SCREEN_W;
        uint16_t *drow = fb + dy * 128;
        for (int dx = 0; dx < 128; dx++) {
            int sx = (dx * 5) / 4;
            uint32_t wx = gb_fit_hw[dx & 3];

            uint32_t a = exp565(r0[sx]);
            uint32_t b = exp565(r0[sx + 1]);
            uint32_t c = exp565(r1[sx]);
            uint32_t d = exp565(r1[sx + 1]);

            uint32_t top = lerp565p(a, b, wx);
            uint32_t bot = lerp565p(c, d, wx);
            uint32_t out = lerp565p(top, bot, wy);

            drow[dx] = pak565(out);
        }
    }
}

/* DMG — palette-aware blend.
 *
 * `shade` is 160×144 bytes, one shade index (0..3) per pixel. `pal`
 * is the 4-entry DMG palette in RGB565.
 *
 * Blend two source shades in /32 fixed-point, giving a scalar in
 * 0..(3*32) = 0..96 covering the full palette range. The integer
 * part selects bracketing palette entries; the fractional part is
 * the lerp weight between them. This keeps output on the palette's
 * own gradient, so a blend never introduces a colour that doesn't
 * lie between two neighbouring palette entries — which is exactly
 * what the user experiences as "green with more blue" on a naive
 * RGB565 blend. */
static void __not_in_flash_func(blit_fit_gb_dmg_blend)(uint16_t *fb,
                                                        const uint8_t *shade,
                                                        const uint16_t *pal) {
    for (int dy = 0; dy < 128; dy++) {
        int sy = (dy * 9) / 8;
        uint32_t wy = gb_fit_vw[dy & 7];
        uint32_t wyi = 32u - wy;
        const uint8_t *r0 = shade + sy * GBC_SCREEN_W;
        const uint8_t *r1 = shade + (sy + 1) * GBC_SCREEN_W;
        uint16_t *drow = fb + dy * 128;
        for (int dx = 0; dx < 128; dx++) {
            int sx = (dx * 5) / 4;
            uint32_t wx = gb_fit_hw[dx & 3];
            uint32_t wxi = 32u - wx;

            /* Horizontal blend in shade-space: (a*wxi + b*wx) gives a
             * fixed-point value in 0..(3*32) = 0..96. */
            uint32_t top = (uint32_t)r0[sx] * wxi + (uint32_t)r0[sx + 1] * wx;
            uint32_t bot = (uint32_t)r1[sx] * wxi + (uint32_t)r1[sx + 1] * wx;
            /* Vertical blend — now in /32 * /32 = /1024 fixed-point
             * over a 0..3 shade range, so max (3*32)*(32) = 3072. */
            uint32_t f = top * wyi + bot * wy;
            /* Collapse: total weight is 32*32 = 1024 per unit shade,
             * so f / 1024 is the integer shade and (f / 32) mod 32 is
             * the 5-bit fractional weight between shade_lo and shade_hi. */
            uint32_t shade_fp = f >> 5;          /* 0..(3*32) */
            uint32_t shade_lo = shade_fp >> 5;   /* 0..3 */
            uint32_t frac     = shade_fp & 31u;  /* /32 */
            uint32_t shade_hi = shade_lo + 1;
            if (shade_hi > 3) shade_hi = 3;

            uint32_t A = exp565(pal[shade_lo]);
            uint32_t B = exp565(pal[shade_hi]);
            uint32_t out = lerp565p(A, B, frac);

            drow[dx] = pak565(out);
        }
    }
}

/* GB 1:1 native crop into a 128x128 window. pan in source coords. */
static void blit_crop_gb(uint16_t *fb, const uint16_t *src,
                          int pan_x, int pan_y) {
    if (pan_x < 0)  pan_x = 0;
    if (pan_x > 32) pan_x = 32;     /* 160 - 128 */
    if (pan_y < 0)  pan_y = 0;
    if (pan_y > 16) pan_y = 16;     /* 144 - 128 */
    for (int dy = 0; dy < 128; dy++) {
        const uint16_t *srow = src + (pan_y + dy) * GBC_SCREEN_W + pan_x;
        uint16_t       *drow = fb + dy * 128;
        for (int dx = 0; dx < 128; dx++) drow[dx] = srow[dx];
    }
}

/* --- per-ROM cfg --------------------------------------------------- */

#define CFG_MAGIC   0x47424345u   /* 'GBCE' */

#define VOL_MIN    0
#define VOL_UNITY 15
#define VOL_MAX  30
#define VOL_DEF  15

typedef enum { SCALE_FIT = 0, SCALE_CROP = 1, SCALE_COUNT } scale_mode_t;

/* Magic flag value that means "blend byte has never been written to
 * this cfg". Distinct from 0 (toggled OFF by user) so we can default
 * new carts to ON while still honouring an explicit user OFF. */
#define BLEND_UNSET  0xFFu

typedef struct {
    uint32_t magic;
    uint8_t  scale_mode;
    uint8_t  show_fps;
    uint8_t  volume;
    uint8_t  palette;
    uint8_t  blend;           /* 0 = off, 1 = on, 0xFF = never written */
    uint8_t  reserved[3];
    uint16_t clock_mhz;       /* 0 = use global; otherwise per-cart override */
    uint16_t _pad2;
} gb_cfg_t;

static void cfg_load(const char *rom_name, scale_mode_t *scale,
                      bool *show_fps, int *volume, int *palette,
                      int *blend, int *clock_mhz) {
    (void)scale;
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return;
    gb_cfg_t c = {0};
    UINT br = 0;
    if (f_read(&f, &c, sizeof(c), &br) == FR_OK && br >= 4
        && c.magic == CFG_MAGIC) {
        if (br >= offsetof(gb_cfg_t, clock_mhz)) {
            *show_fps = c.show_fps != 0;
            if (c.volume <= VOL_MAX)           *volume = c.volume;
            if (c.palette < GBC_PALETTE_COUNT) *palette = c.palette;
            /* Blend byte is a late addition — pre-1.04 cfgs have a
             * zero in this slot (used to be reserved[0]). Treat 0xFF
             * as "never written" (→ caller's default stays). We write
             * 1 (on) on subsequent saves so the first save after
             * launching the cart normalises old cfgs to the new
             * default. */
            if (blend && c.blend != BLEND_UNSET) {
                *blend = c.blend ? 1 : 0;
            }
        }
        if (clock_mhz && br >= sizeof(c)) {
            *clock_mhz = c.clock_mhz;
        }
    }
    f_close(&f);
}

static void cfg_save(const char *rom_name, scale_mode_t scale,
                      bool show_fps, int volume, int palette,
                      int blend, int clock_mhz) {
    (void)scale;
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    gb_cfg_t c = {
        .magic = CFG_MAGIC, .scale_mode = (uint8_t)SCALE_FIT,
        .show_fps = show_fps ? 1 : 0, .volume = (uint8_t)volume,
        .palette = (uint8_t)palette,
        .blend = (uint8_t)(blend ? 1 : 0),
        .reserved = {0,0,0},
        .clock_mhz = (uint16_t)clock_mhz, ._pad2 = 0,
    };
    UINT bw = 0;
    f_write(&f, &c, sizeof(c), &bw);
    f_close(&f);
}

int gb_run_clock_override(const char *rom_name) {
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;
    gb_cfg_t c = {0};
    UINT br = 0;
    f_read(&f, &c, sizeof(c), &br);
    f_close(&f);
    if (br < sizeof(c) || c.magic != CFG_MAGIC) return 0;
    return (int)c.clock_mhz;
}

/* See nes_run.c for the rationale — VOL_UNITY=15 is 1.0x and
 * VOL_MAX=30 is 2.0x with hard clipping at the int16 boundary. */
static void scale_audio(int16_t *buf, int n, int volume) {
    if (volume == VOL_UNITY) return;
    if (volume <= 0) { for (int i = 0; i < n; i++) buf[i] = 0; return; }
    for (int i = 0; i < n; i++) {
        int32_t s = (int32_t)buf[i] * volume / VOL_UNITY;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
}

int gb_run_rom(const nes_rom_entry *e, uint16_t *fb) {
    const char *name = e->name;

    /* Reuse the picker's XIP mmap path so multi-MB cart ROMs don't
     * touch heap. We capture the mmap return code so the failure
     * splash can tell us which leg of the load tripped:
     *   -32 -> mmap f_open failed
     *   -33 -> mmap rejected file size
     *   -34 -> mmap saw start_cluster < 2
     *   -35 -> mmap chain_is_contiguous returned false (the
     *          fragmentation case the defragmenter targets) */
    const uint8_t *rom_const = NULL;
    uint8_t       *rom_alloc = NULL;
    size_t         sz        = 0;
    /* -5 (chain not contiguous) routes to chained-XIP, which reads
     * the fragmented file straight out of flash via a per-cluster
     * LBA table. Any other mmap failure falls through to the RAM
     * load fallback. */
    nes_picker_rom_chain_t chain = {0};
    bool use_chain = false;
    int  mmap_rc = nes_picker_mmap_rom(name, &rom_const, &sz);
    if (mmap_rc == -5) {
        if (nes_picker_mmap_rom_chain(name, &chain) == 0) {
            sz        = chain.size;
            use_chain = true;
            mmap_rc   = 0;
        }
    }
    if (mmap_rc != 0) {
        rom_alloc = nes_picker_load_rom(name, &sz);
        if (!rom_alloc) return -30 + mmap_rc;   /* -32 .. -35 */
        rom_const = rom_alloc;
    }

    if (gbc_init(22050) != 0) {
        free(rom_alloc);
        nes_picker_mmap_rom_chain_free(&chain);
        return -20;
    }
    int load_rc = use_chain
        ? gbc_load_rom_chain(chain.cluster_ptrs,
                              chain.cluster_shift,
                              chain.cluster_mask, sz)
        : gbc_load_rom(rom_const, sz);
    if (load_rc != 0) {
        free(rom_alloc);
        nes_picker_mmap_rom_chain_free(&chain);
        /* Translate the wrapper code to a device-side label so the
         * user can tell why the cart was rejected:
         *   -1 -> -10 "bad header / too small"
         *   -2 -> -11 "MBC unsupported (peanut_gb)"
         *   -3 -> -12 "header checksum mismatch"  */
        if (load_rc == -1) return -10;
        if (load_rc == -2) return -11;
        if (load_rc == -3) return -12;
        return -13;
    }
    battery_load(name);

    int          volume       = VOL_DEF;
    int          palette      = GBC_PALETTE_GREEN;
    bool         show_fps     = false;
    bool         fast_forward = false;
    scale_mode_t scale_mode   = SCALE_FIT;
    int  cart_clock_mhz = 0;
    /* BLEND default: ON for both DMG and CGB. Nearest is jarring at
     * the 5:4 × 9:8 ratio (columns drop) so we want coverage blend by
     * default, with the per-cart BLEND toggle for users who prefer
     * the old sharper look on specific carts. cfg_load leaves this
     * alone for pre-BLEND cfgs (0xFF sentinel), so old carts land on
     * the default until the user touches the menu. */
    int  blend = 1;
    cfg_load(name, &scale_mode, &show_fps, &volume, &palette, &blend, &cart_clock_mhz);
    /* Volume is global across all carts now — pull it from /.global. */
    volume = nes_picker_global_volume();
    gbc_set_palette(palette);
    bool cfg_dirty = false;

    int  osd_text_ms  = 0;
    char osd_text[24] = {0};

    const uint64_t AUTOSAVE_INTERVAL_US = 30u * 1000u * 1000u;
    uint64_t       last_autosave_us     = (uint64_t)time_us_64();
    int            unsaved_play_frames  = 0;

    const int      IDLE_SLEEP_S = 90;
    uint64_t       last_input_us = (uint64_t)time_us_64();
    bool           sleeping = false;

    /* Palette lookup is baked into the core's framebuffer now — the
     * blitters do a pure word copy. gbc_set_palette still updates the
     * DMG-mode palette used by gb_core internally. */

    int  menu_press_ms = 0;
    int  menu_was_down = 0;
    int  menu_consumed = 0;
    int  open_menu     = 0;
    bool exit_after    = false;

    int pan_x = 16, pan_y = 8;
    int prev_lb = 0, prev_rb = 0, prev_up = 0, prev_dn = 0;
    int prev_lt = 0, prev_rt = 0, prev_a = 0;

    int16_t audio[1024];

    const uint32_t FRAME_US = 1000000u / (uint32_t)gbc_refresh_rate();
    absolute_time_t next_frame = get_absolute_time();
    absolute_time_t fps_window = get_absolute_time();
    int fps_frames = 0, fps_show = 0;

    while (!exit_after) {
        int menu_down = !gpio_get(BTN_MENU_GP);
        int lb_down = !gpio_get(BTN_LB_GP);
        int rb_down = !gpio_get(BTN_RB_GP);
        int up_down = !gpio_get(BTN_UP_GP);
        int dn_down = !gpio_get(BTN_DOWN_GP);
        int lt_down = !gpio_get(BTN_LEFT_GP);
        int rt_down = !gpio_get(BTN_RIGHT_GP);
        int a_down  = !gpio_get(BTN_A_GP);
        int any_input = menu_down || lb_down || rb_down || up_down || dn_down
                        || lt_down || rt_down
                        || a_down || !gpio_get(BTN_B_GP);

        if (any_input) {
            last_input_us = (uint64_t)time_us_64();
            if (sleeping) {
                /* Re-anchor frame pacing on wake — see nes_run.c
                 * for the full reasoning. */
                sleeping = false;
                nes_lcd_backlight(1);
                next_frame = get_absolute_time();
            }
        }
        if (!sleeping &&
            (uint64_t)time_us_64() - last_input_us > (uint64_t)IDLE_SLEEP_S * 1000000u) {
            battery_save(name);
            unsaved_play_frames = 0;
            sleeping = true;
            nes_lcd_backlight(0);
        }
        if (sleeping) { sleep_ms(50); continue; }

        if (menu_down) {
            menu_press_ms += 16;
            menu_was_down = 1;
            /* MENU+A: snapshot framebuffer to .scr32 + .scr64. */
            if (a_down && !prev_a) {
                int rc = nes_thumb_save(fb, name);
                if (rc == 0) nes_flash_disk_flush();
                snprintf(osd_text, sizeof(osd_text),
                          rc == 0 ? "shot saved" : "shot fail");
                osd_text_ms = 800;
                menu_consumed = 1;
            }
            /* CROP mode: cart keeps running, MENU+dpad pans the
             * viewport continuously. */
            if (scale_mode == SCALE_CROP) {
                const int PAN_STEP = 1;
                if (up_down) { pan_y -= PAN_STEP; menu_consumed = 1; }
                if (dn_down) { pan_y += PAN_STEP; menu_consumed = 1; }
                if (lt_down) { pan_x -= PAN_STEP; menu_consumed = 1; }
                if (rt_down) { pan_x += PAN_STEP; menu_consumed = 1; }
                if (pan_x < 0)  pan_x = 0;
                if (pan_x > 32) pan_x = 32;
                if (pan_y < 0)  pan_y = 0;
                if (pan_y > 16) pan_y = 16;
            }
            /* MENU long hold (>= 500 ms with no chord) opens the
             * in-game menu. */
            if (menu_press_ms >= 500 && !menu_consumed) {
                open_menu = 1;
                menu_consumed = 1;
            }
        } else {
            if (menu_was_down) {
                if (!menu_consumed && menu_press_ms > 0 && menu_press_ms < 300) {
                    scale_mode = (scale_mode_t)((scale_mode + 1) % SCALE_COUNT);
                    cfg_dirty = true;
                    if (scale_mode == SCALE_CROP) { pan_x = 16; pan_y = 8; }
                }
                menu_press_ms = 0;
                menu_was_down = 0;
                menu_consumed = 0;
            }
        }
        prev_lb = lb_down; prev_rb = rb_down;
        prev_up = up_down; prev_dn = dn_down;
        prev_lt = lt_down; prev_rt = rt_down;
        prev_a  = a_down;

        /* ----- in-game menu ----- */
        if (open_menu) {
            open_menu = 0;
            int v_scale = (int)scale_mode;
            int v_vol   = volume;
#ifdef THUMBYONE_SLOT_MODE
            int v_bri   = thumbyone_settings_load_brightness();
            int old_bri = v_bri;
#endif
            int v_ff    = fast_forward ? 1 : 0;
            int v_fps   = show_fps ? 1 : 0;
            int v_pal   = palette;
            int v_blend = blend ? 1 : 0;
            int v_clock = 0;
            if (cart_clock_mhz == 125) v_clock = 1;
            if (cart_clock_mhz == 150) v_clock = 2;
            if (cart_clock_mhz == 200) v_clock = 3;
            if (cart_clock_mhz == 250) v_clock = 4;
            /* 300 MHz removed for stability — saved configs with 300
             * fall back to "global" (v_clock=0) silently. */

            static const char * const display_choices[] = { "FIT", "CROP" };
            static const char * const clock_choices[]   = { "global","125MHz","150MHz","200MHz","250MHz" };
            static const int          clock_mhz_arr[]   = {  0,       125,     150,     200,     250 };
            static const char * const palette_names_arr[] = {
                "GREEN", "GREY", "POCKET", "CREAM", "BLUE", "RED",
            };

            enum { ACT_NONE, ACT_SAVE_STATE, ACT_LOAD_STATE, ACT_QUIT };

            char sta_path[NES_PICKER_PATH_MAX];
            make_sidecar_path(sta_path, sizeof(sta_path), name, ".sta");
            FIL _f;
            bool sta_exists = (f_open(&_f, sta_path, FA_READ) == FR_OK);
            if (sta_exists) f_close(&_f);

            /* Build the items list imperatively so the optional
             * "fragmented" info row — only shown when the current
             * session is running the chained-XIP fallback — can slot
             * in without a surprise noise row on healthy carts. */
            nes_menu_item_t items[16];
            int n_items = 0;
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_ACTION,
                  .label = "Resume", .enabled = true, .action_id = ACT_NONE };
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_ACTION,
                  .label = "Save state", .enabled = true, .action_id = ACT_SAVE_STATE };
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_ACTION,
                  .label = "Load state", .enabled = sta_exists, .action_id = ACT_LOAD_STATE };
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_CHOICE,
                  .label = "Display", .value_ptr = &v_scale,
                  .choices = display_choices, .num_choices = 2, .enabled = true };
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_SLIDER,
                  .label = "Volume", .value_ptr = &v_vol,
                  .min = VOL_MIN, .max = VOL_MAX, .enabled = true };
#ifdef THUMBYONE_SLOT_MODE
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_SLIDER,
                  .label = "Brightness", .value_ptr = &v_bri,
                  .min = 0, .max = 255, .enabled = true };
#endif
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_TOGGLE,
                  .label = "Fast-fwd", .value_ptr = &v_ff, .enabled = true };
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_TOGGLE,
                  .label = "Show FPS", .value_ptr = &v_fps, .enabled = true };
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_CHOICE,
                  .label = "Palette", .value_ptr = &v_pal,
                  .choices = palette_names_arr,
                  .num_choices = GBC_PALETTE_COUNT, .enabled = true };
            /* BLEND — toggles FIT between asymmetric 5:4 × 9:8 nearest
             * and a coverage-weighted blend (palette-aware on DMG,
             * RGB565 packed-lerp on CGB). Not applicable in CROP. */
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_TOGGLE,
                  .label = "Blend", .value_ptr = &v_blend,
                  .enabled = (scale_mode == SCALE_FIT) };
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_CHOICE,
                  .label = "Overclock", .value_ptr = &v_clock,
                  .choices = clock_choices, .num_choices = 5,
                  .enabled = true, .suffix = "next launch" };
            if (use_chain) {
                /* Running from the chained-XIP fallback — ROM file is
                 * fragmented on flash. Tell the user a defragment
                 * might recover the contiguous fast path. */
                items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_INFO,
                      .label = "Storage", .info_text = "fragmented",
                      .enabled = true };
            }
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_ACTION,
                  .label = "Quit to picker", .enabled = true, .action_id = ACT_QUIT };

            char sub[28];
            strncpy(sub, name, sizeof(sub) - 1);
            sub[sizeof(sub) - 1] = 0;
            char *dot = strrchr(sub, '.');
            if (dot) *dot = 0;

            nes_menu_result_t r = nes_menu_run(fb, "PAUSED", sub, items, n_items);

            if (v_scale != (int)scale_mode) {
                scale_mode = (scale_mode_t)v_scale;
                if (scale_mode == SCALE_CROP) { pan_x = 16; pan_y = 8; }
                cfg_dirty = true;
            }
            if (v_vol   != volume       ) {
                volume = v_vol;
                nes_picker_global_set_volume(v_vol);
            }
#ifdef THUMBYONE_SLOT_MODE
            if (v_bri != old_bri) {
                if (v_bri < 0)   v_bri = 0;
                if (v_bri > 255) v_bri = 255;
                thumbyone_settings_save_brightness((uint8_t)v_bri);
                nes_flash_disk_flush();
                thumbyone_backlight_set((uint8_t)v_bri);
            }
#endif
            if ((bool)v_ff    != fast_forward) { fast_forward = (bool)v_ff;       }
            if ((bool)v_fps   != show_fps    ) { show_fps     = (bool)v_fps; cfg_dirty = true; }
            if (v_pal != palette) {
                palette = v_pal;
                gbc_set_palette(palette);
                cfg_dirty = true;
            }
            if (v_blend != blend) {
                blend = v_blend;
                cfg_dirty = true;
            }
            int new_mhz = clock_mhz_arr[v_clock];
            if (new_mhz != cart_clock_mhz) {
                cart_clock_mhz = new_mhz;
                cfg_dirty = true;
                if (new_mhz == 0) {
                    snprintf(osd_text, sizeof(osd_text), "clock: global");
                } else {
                    snprintf(osd_text, sizeof(osd_text), "clock %d (next)", new_mhz);
                }
                osd_text_ms = 1500;
            }

            if (r.kind == NES_MENU_ACTION) {
                switch (r.action_id) {
                case ACT_SAVE_STATE: {
                    int rc = gbc_save_state(sta_path);
                    nes_flash_disk_flush();
                    snprintf(osd_text, sizeof(osd_text),
                              rc == 0 ? "state saved" : "save fail");
                    osd_text_ms = 1000;
                    break;
                }
                case ACT_LOAD_STATE: {
                    int rc = gbc_load_state(sta_path);
                    snprintf(osd_text, sizeof(osd_text),
                              rc == 0 ? "state loaded" : "load fail");
                    osd_text_ms = 1000;
                    break;
                }
                case ACT_QUIT:
                    exit_after = true;
                    break;
                }
            }

            next_frame = get_absolute_time();
            last_input_us = (uint64_t)time_us_64();
        }

        /* The cart always receives input — even in CROP mode. The
         * pan controls live on MENU+dpad above. */
        gbc_set_buttons(menu_down ? 0 : read_gb_buttons());

        /* And the cart always ticks. */
        {
            int frame_runs = fast_forward ? 4 : 1;
            for (int i = 0; i < frame_runs; i++) gbc_run_frame();
            unsaved_play_frames += frame_runs;
        }

        const uint16_t *frame = gbc_framebuffer();
        if (frame) {
            nes_lcd_wait_idle();
            if (scale_mode == SCALE_CROP) {
                blit_crop_gb(fb, frame, pan_x, pan_y);
            } else if (!blend) {
                blit_fit_gb_nearest(fb, frame);
            } else if (gbc_is_cgb_cart()) {
                blit_fit_gb_cgb_blend(fb, frame);
            } else {
                /* DMG path needs the shade buffer; fall back to
                 * RGB565 blend if it wasn't allocated (OOM at
                 * load time — graceful degrade). */
                const uint8_t *shade = gbc_shade_buffer();
                const uint16_t *pal = gbc_palette_rgb565();
                if (shade && pal) blit_fit_gb_dmg_blend(fb, shade, pal);
                else              blit_fit_gb_cgb_blend(fb, frame);
            }
            if (show_fps) {
                char ftxt[12];
                snprintf(ftxt, sizeof(ftxt), "%d%s", fps_show,
                         fast_forward ? " FF" : "");
                nes_font_draw(fb, ftxt, 2, 5, 0xFFE0);
            }
            if (osd_text_ms > 0) {
                int w = nes_font_width(osd_text);
                nes_font_draw(fb, osd_text, (128 - w) / 2, 60, 0xFFE0);
                osd_text_ms -= 16;
            }
            nes_lcd_present(fb);
        }

        {
            int n = gbc_audio_pull(audio, 1024);
            if (n > 0) {
                scale_audio(audio, n, volume);
                nes_audio_pwm_push(audio, n);
            }
        }

        if (unsaved_play_frames > 0 &&
            (uint64_t)time_us_64() - last_autosave_us > AUTOSAVE_INTERVAL_US) {
            battery_save(name);
            last_autosave_us    = (uint64_t)time_us_64();
            unsaved_play_frames = 0;
        }

        fps_frames++;
        if (!fast_forward) {
            next_frame = delayed_by_us(next_frame, FRAME_US);
            sleep_until(next_frame);
        } else {
            next_frame = get_absolute_time();
        }
        if (absolute_time_diff_us(fps_window, get_absolute_time()) > 1000000) {
            fps_show = fps_frames; fps_frames = 0;
            fps_window = get_absolute_time();
        }
    }

    battery_save(name);
    if (cfg_dirty) cfg_save(name, scale_mode, show_fps, volume, palette, blend, cart_clock_mhz);
    nes_lcd_backlight(1);
    gbc_shutdown();
    free(rom_alloc);
    nes_picker_mmap_rom_chain_free(&chain);
    while (!gpio_get(BTN_MENU_GP)) sleep_ms(10);
    return 0;
}
