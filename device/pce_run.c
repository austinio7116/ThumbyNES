/*
 * ThumbyNES — device-side PC Engine runner.
 *
 * Mirrors gb_run.c / md_run.c / nes_run.c shape. Reuses every hardware
 * driver as-is (LCD, audio PWM, buttons, FAT battery sidecars, in-game
 * menu, idle-sleep, autosave). The PCE-specific bits are the scaler
 * set (256×224 → 128×112 with letterbox) and the controller mapping.
 *
 * NOT SAFE ON DEVICE YET. HuExpress allocates a 220 KB XBUF for tile
 * + sprite rendering which will OOM on the RP2350 alongside the other
 * cores. This runner compiles clean under THUMBYNES_WITH_PCE=ON but
 * the final flashable image is blocked on the scanline renderer port
 * (task #12 / PCE_PLAN.md §5), which replaces the XBUF with a ~2 KB
 * line scratch feeding directly into this fb. The scalers below are
 * written in the format the scanline path will produce once it lands,
 * so this code does not need to change when the swap happens.
 */
#include "pce_run.h"
#include "pce_core.h"
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

/* Pin map mirrors nes_run.c / gb_run.c. */
#define BTN_LEFT_GP   0
#define BTN_UP_GP     1
#define BTN_RIGHT_GP  2
#define BTN_DOWN_GP   3
#define BTN_LB_GP     6
#define BTN_A_GP     21
#define BTN_RB_GP    22
#define BTN_B_GP     25
#define BTN_MENU_GP  26

/* PCE natively 256×224 (most carts). The core's framebuffer is 600
 * wide (XBUF padding) so stride = PCEC_PITCH. Active viewport sits
 * at (0,0); we clip anything wider at 256. */
#define PCE_SRC_W   256
#define PCE_SRC_H   224
#define PCE_FIT_H   (PCE_SRC_H / 2)      /* 112 — 8 px letterbox top & bottom */

/* Second LCD-shaped framebuffer for double-buffered DMA. The scanline
 * renderer writes directly to whichever buffer is not currently being
 * shipped to the GC9107, so emu+render of frame N overlaps the DMA of
 * frame N-1. Without this, every frame stalls ~10 ms on
 * nes_lcd_wait_idle before it can touch fb. 32 KB BSS, only allocated
 * when the PCE slot is built. */
static uint16_t pce_fb_back[128 * 128];

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
    uint8_t *ram = pcec_battery_ram();
    size_t   sz  = pcec_battery_size();
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
    uint8_t *ram = pcec_battery_ram();
    size_t   sz  = pcec_battery_size();
    if (!ram || sz == 0) return;
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".sav");
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT bw = 0;
    f_write(&f, ram, (UINT)sz, &bw);
    f_close(&f);
}

/* PCE joypad bitmask from Thumby buttons. MENU is reserved for the
 * in-game menu (long-hold) and gets masked out of the cart's view
 * when held; we can't reuse it for RUN. Map shoulders instead:
 *   LB → SELECT,  RB → RUN. */
static uint16_t read_pce_buttons(void) {
    uint16_t m = 0;
    if (!gpio_get(BTN_A_GP))     m |= PCEC_BTN_I;
    if (!gpio_get(BTN_B_GP))     m |= PCEC_BTN_II;
    if (!gpio_get(BTN_LB_GP))    m |= PCEC_BTN_SELECT;
    if (!gpio_get(BTN_RB_GP))    m |= PCEC_BTN_RUN;
    if (!gpio_get(BTN_UP_GP))    m |= PCEC_BTN_UP;
    if (!gpio_get(BTN_DOWN_GP))  m |= PCEC_BTN_DOWN;
    if (!gpio_get(BTN_LEFT_GP))  m |= PCEC_BTN_LEFT;
    if (!gpio_get(BTN_RIGHT_GP)) m |= PCEC_BTN_RIGHT;
    return m;
}

/* --- scalers ------------------------------------------------------- *
 *
 * Three blits, selected by scale_mode + blend toggle (same structure
 * as nes_run.c / gb_run.c):
 *
 *   blit_fit_pce    — 2:1 nearest, 8-row letterbox top & bottom.
 *                     Sharp pixels, drops every 2nd source column
 *                     and row. Used when BLEND is OFF.
 *   blit_blend_pce  — 2×2 box average in packed-RGB565 space. Each
 *                     output pixel averages four source indices
 *                     through the palette LUT. Used when BLEND is ON.
 *   blit_crop_pce   — 1:1 native 128×128 viewport into the 256×224
 *                     frame, pannable with MENU + d-pad. Max pan is
 *                     (128, 96).
 *
 * Source format: palette indices, stride = PCEC_PITCH bytes, 224
 * visible rows starting at (0, 0). The core's pcec_palette_rgb565()
 * returns a 256-entry RGB565 LUT keyed on those indices (see the
 * VCE packed-byte encoding in pce_core.c::build_palette_once).
 */

/* 256×224 → 128×112, 2:1 nearest with 8-row letterbox top and bottom. */
static void blit_fit_pce(uint16_t *fb, const uint8_t *src,
                          const uint16_t *pal) {
    memset(fb, 0, 128 * 8 * 2);
    for (int dy = 0; dy < PCE_FIT_H; dy++) {
        const uint8_t *srow = src + (dy * 2) * PCEC_PITCH;
        uint16_t      *drow = fb + (8 + dy) * 128;
        for (int dx = 0; dx < 128; dx++) {
            drow[dx] = pal[srow[dx * 2]];
        }
    }
    memset(fb + (8 + PCE_FIT_H) * 128, 0, 128 * 8 * 2);
}

/* 256×224 → 128×112 with 2×2 box average per output pixel. Four source
 * palette indices per destination → four pal lookups → channel-split
 * sum / 4 in packed RGB565 bit positions. Same letterbox as FIT. */
static void blit_blend_pce(uint16_t *fb, const uint8_t *src,
                            const uint16_t *pal) {
    memset(fb, 0, 128 * 8 * 2);
    for (int dy = 0; dy < PCE_FIT_H; dy++) {
        const uint8_t *r0 = src + (dy * 2)     * PCEC_PITCH;
        const uint8_t *r1 = src + (dy * 2 + 1) * PCEC_PITCH;
        uint16_t      *drow = fb + (8 + dy) * 128;
        for (int dx = 0; dx < 128; dx++) {
            int sx = dx * 2;
            uint16_t a = pal[r0[sx]];
            uint16_t b = pal[r0[sx + 1]];
            uint16_t c = pal[r1[sx]];
            uint16_t d = pal[r1[sx + 1]];
            /* Sum each channel across the 4 source pixels.
             * Max channel sum: R 4*31=124, G 4*63=252, B 4*31=124. */
            uint32_t rsum = ((a >> 11) & 0x1F) + ((b >> 11) & 0x1F)
                          + ((c >> 11) & 0x1F) + ((d >> 11) & 0x1F);
            uint32_t gsum = ((a >>  5) & 0x3F) + ((b >>  5) & 0x3F)
                          + ((c >>  5) & 0x3F) + ((d >>  5) & 0x3F);
            uint32_t bsum = (a & 0x1F) + (b & 0x1F)
                          + (c & 0x1F) + (d & 0x1F);
            drow[dx] = (uint16_t)(((rsum >> 2) << 11)
                                | ((gsum >> 2) <<  5)
                                |  (bsum >> 2));
        }
    }
    memset(fb + (8 + PCE_FIT_H) * 128, 0, 128 * 8 * 2);
}

/* 1:1 native crop. Copies a 128×128 window starting at (pan_x, pan_y),
 * clamped so the window stays inside the 256×224 active area. */
static void blit_crop_pce(uint16_t *fb, const uint8_t *src,
                           const uint16_t *pal, int pan_x, int pan_y) {
    if (pan_x < 0)   pan_x = 0;
    if (pan_x > 128) pan_x = 128;        /* 256 - 128 */
    if (pan_y < 0)   pan_y = 0;
    if (pan_y > 96)  pan_y = 96;         /* 224 - 128 */
    for (int dy = 0; dy < 128; dy++) {
        const uint8_t *srow = src + (pan_y + dy) * PCEC_PITCH + pan_x;
        uint16_t      *drow = fb + dy * 128;
        for (int dx = 0; dx < 128; dx++) {
            drow[dx] = pal[srow[dx]];
        }
    }
}

/* --- per-ROM cfg --------------------------------------------------- */

#define CFG_MAGIC   0x50434558u   /* 'PCEX' */

#define VOL_MIN    0
#define VOL_UNITY 15
#define VOL_MAX  30
#define VOL_DEF  15

typedef enum { SCALE_FIT = 0, SCALE_CROP = 1, SCALE_COUNT } scale_mode_t;

#define BLEND_UNSET  0xFFu

typedef struct {
    uint32_t magic;
    uint8_t  scale_mode;
    uint8_t  show_fps;
    uint8_t  volume;
    uint8_t  blend;            /* 0 = off, 1 = on, 0xFF = never written */
    uint8_t  reserved[4];
    uint16_t clock_mhz;        /* 0 = use global; otherwise 125/150/200/250 */
    uint16_t _pad2;
} pce_cfg_t;

static void cfg_load(const char *rom_name, scale_mode_t *scale,
                      bool *show_fps, int *volume,
                      int *blend, int *clock_mhz) {
    (void)scale;   /* scale is intentionally NOT restored — carts boot
                    * in FIT; user toggles to CROP by hand. Avoids
                    * opening into a zoomed view that may not match
                    * what the game renders in the first frames. */
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return;
    pce_cfg_t c = {0};
    UINT br = 0;
    if (f_read(&f, &c, sizeof(c), &br) == FR_OK && br >= 4
        && c.magic == CFG_MAGIC) {
        if (br >= offsetof(pce_cfg_t, clock_mhz)) {
            *show_fps = c.show_fps != 0;
            if (c.volume <= VOL_MAX) *volume = c.volume;
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
                      bool show_fps, int volume,
                      int blend, int clock_mhz) {
    (void)scale;
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    pce_cfg_t c = {
        .magic = CFG_MAGIC, .scale_mode = (uint8_t)SCALE_FIT,
        .show_fps = show_fps ? 1 : 0, .volume = (uint8_t)volume,
        .blend = (uint8_t)(blend ? 1 : 0),
        .reserved = {0,0,0,0},
        .clock_mhz = (uint16_t)clock_mhz, ._pad2 = 0,
    };
    UINT bw = 0;
    f_write(&f, &c, sizeof(c), &bw);
    f_close(&f);
}

int pce_run_clock_override(const char *rom_name) {
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;
    pce_cfg_t c = {0};
    UINT br = 0;
    f_read(&f, &c, sizeof(c), &br);
    f_close(&f);
    if (br < sizeof(c) || c.magic != CFG_MAGIC) return 0;
    return (int)c.clock_mhz;
}

/* Shared with other runners — VOL_UNITY=15 is 1.0x, VOL_MAX=30 is 2.0x
 * with int16 clipping. Zero = mute. */
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

int pce_run_rom(const nes_rom_entry *e, uint16_t *fb) {
    const char *name = e->name;

    /* Reuse the picker's XIP mmap path. PCE HuCards are small enough
     * (max 1 MB common, 2.5 MB for 20-Mbit carts) that fragmented
     * chains are rare but we still fall through to the RAM loader if
     * mmap rejects the file. */
    const uint8_t *rom_const = NULL;
    uint8_t       *rom_alloc = NULL;
    size_t         sz        = 0;
    int mmap_rc = nes_picker_mmap_rom(name, &rom_const, &sz);
    if (mmap_rc != 0) {
        rom_alloc = nes_picker_load_rom(name, &sz);
        if (!rom_alloc) return -30 + mmap_rc;
        rom_const = rom_alloc;
    }

    /* US-encoded (bit-reversed) HuCards aren't supported on the device.
     * Pre-decoded dumps work fine and are the de-facto standard in
     * modern PCE/TG-16 dump sets, so we just refuse to load encrypted
     * blobs and surface a load-error in the picker. */
    if (pcec_rom_is_us_encoded(rom_const, sz)) {
        free(rom_alloc);
        return -22;     /* surfaces as "load err -22" in nes_device_main */
    }

    if (pcec_init(PCEC_REGION_AUTO, 22050) != 0) {
        free(rom_alloc);
        return -20;
    }
    if (pcec_load_rom(rom_const, sz) != 0) {
        free(rom_alloc);
        pcec_shutdown();
        return -10;
    }
    battery_load(name);

    int          volume       = VOL_DEF;
    bool         show_fps     = false;
    bool         fast_forward = false;
    scale_mode_t scale_mode   = SCALE_FIT;
    int          cart_clock_mhz = 0;
    int          blend = 1;     /* BLEND ON by default — 2:1 is cleaner
                                 * with a box average than raw drop. */
    cfg_load(name, &scale_mode, &show_fps, &volume, &blend, &cart_clock_mhz);
    volume = nes_picker_global_volume();
    bool cfg_dirty = false;

    int  osd_text_ms  = 0;
    char osd_text[24] = {0};

    const uint64_t AUTOSAVE_INTERVAL_US = 30u * 1000u * 1000u;
    uint64_t       last_autosave_us     = (uint64_t)time_us_64();
    int            unsaved_play_frames  = 0;

    const int IDLE_SLEEP_S = 90;
    uint64_t  last_input_us = (uint64_t)time_us_64();
    bool      sleeping = false;

    int  menu_press_ms = 0;
    int  menu_was_down = 0;
    int  menu_consumed = 0;
    int  open_menu     = 0;
    bool exit_after    = false;

    /* Tracks which buffer was last DMA'd to the LCD. The menu paints
     * on `fb`, so we copy back when opening the menu if the last
     * frame ended up on pce_fb_back. */
    uint16_t *last_presented_fb = fb;
    /* Buffer-selection toggle. Must NOT reuse fps_frames here — that
     * counter resets every 1-sec FPS window and would collapse the
     * ping-pong (two consecutive frames sharing the same buffer →
     * render races the in-flight DMA → ghosting + flicker). */
    int       fb_toggle = 0;

    int pan_x = 64, pan_y = 48;       /* 256-128=128 /2, 224-128=96 /2 → centred */
    int prev_a = 0;

    int16_t audio[1024];

    const uint32_t FRAME_US = 1000000u / (uint32_t)pcec_refresh_rate();
    absolute_time_t next_frame = get_absolute_time();
    absolute_time_t fps_window = get_absolute_time();
    int fps_frames = 0, fps_show = 0;

    /* Adaptive frameskip — same shape as md_run.c. Skip the scanline
     * composer when emulation alone overran last frame's budget,
     * with a hard cap so we don't freeze the display for long. */
    uint32_t prev_emu_us     = 0;
    uint32_t prev_cycle_us   = 0;     /* end-to-end loop iter time */
    uint32_t prev_wait_us    = 0;     /* DMA-wait at top of cycle */
    uint32_t prev_aud_us     = 0;     /* audio pull + push */
    uint32_t cycle_us_show   = 0;
    uint32_t wait_us_show    = 0;
    uint32_t aud_us_show     = 0;
    uint64_t prev_cycle_t0   = (uint64_t)time_us_64();
    uint32_t skip_streak     = 0;
    uint32_t skipped_count   = 0;
    uint32_t skipped_show    = 0;
    uint32_t emu_us_show     = 0;
    const int SKIP_BUDGET_US = (int)FRAME_US;
    const int SKIP_STREAK_MAX = 2;

    while (!exit_after) {
        int menu_down = !gpio_get(BTN_MENU_GP);
        int up_down   = !gpio_get(BTN_UP_GP);
        int dn_down   = !gpio_get(BTN_DOWN_GP);
        int lt_down   = !gpio_get(BTN_LEFT_GP);
        int rt_down   = !gpio_get(BTN_RIGHT_GP);
        int a_down    = !gpio_get(BTN_A_GP);
        int any_input = menu_down || up_down || dn_down || lt_down || rt_down
                        || a_down || !gpio_get(BTN_B_GP)
                        || !gpio_get(BTN_LB_GP) || !gpio_get(BTN_RB_GP);

        if (any_input) {
            last_input_us = (uint64_t)time_us_64();
            if (sleeping) {
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
            /* MENU+A: framebuffer snapshot. */
            if (a_down && !prev_a) {
                int rc = nes_thumb_save(fb, name);
                snprintf(osd_text, sizeof(osd_text),
                          rc == 0 ? "shot saved" : "shot fail");
                osd_text_ms = 800;
                menu_consumed = 1;
            }
            /* CROP pan via MENU + d-pad. */
            if (scale_mode == SCALE_CROP) {
                const int PAN_STEP = 2;
                if (up_down) { pan_y -= PAN_STEP; menu_consumed = 1; }
                if (dn_down) { pan_y += PAN_STEP; menu_consumed = 1; }
                if (lt_down) { pan_x -= PAN_STEP; menu_consumed = 1; }
                if (rt_down) { pan_x += PAN_STEP; menu_consumed = 1; }
                if (pan_x < 0)   pan_x = 0;
                if (pan_x > 128) pan_x = 128;
                if (pan_y < 0)   pan_y = 0;
                if (pan_y > 96)  pan_y = 96;
            }
            /* Long-hold MENU (≥ 500 ms, no chord) opens in-game menu. */
            if (menu_press_ms >= 500 && !menu_consumed) {
                open_menu = 1;
                menu_consumed = 1;
            }
        } else {
            if (menu_was_down) {
                if (!menu_consumed && menu_press_ms > 0 && menu_press_ms < 300) {
                    scale_mode = (scale_mode_t)((scale_mode + 1) % SCALE_COUNT);
                    cfg_dirty = true;
                    if (scale_mode == SCALE_CROP) { pan_x = 64; pan_y = 48; }
                }
                menu_press_ms = 0;
                menu_was_down = 0;
                menu_consumed = 0;
            }
        }
        prev_a = a_down;

        /* ----- in-game menu ----- */
        if (open_menu) {
            open_menu = 0;
            /* Menu paints on fb, but our last gameplay frame might
             * be sitting on pce_fb_back. Copy it across so the menu
             * shows on top of the actual game state. ~150 us. */
            if (last_presented_fb != fb) {
                nes_lcd_wait_idle();
                memcpy(fb, last_presented_fb, 128 * 128 * 2);
            }
            int v_scale = (int)scale_mode;
            int v_vol   = volume;
#ifdef THUMBYONE_SLOT_MODE
            int v_bri   = thumbyone_settings_load_brightness();
            int old_bri = v_bri;
#endif
            int v_ff    = fast_forward ? 1 : 0;
            int v_fps   = show_fps ? 1 : 0;
            int v_blend = blend ? 1 : 0;
            int v_clock = 0;
            if (cart_clock_mhz == 125) v_clock = 1;
            if (cart_clock_mhz == 150) v_clock = 2;
            if (cart_clock_mhz == 200) v_clock = 3;
            if (cart_clock_mhz == 250) v_clock = 4;

            static const char * const display_choices[] = { "FIT", "CROP" };
            static const char * const clock_choices[]   = { "global","125MHz","150MHz","200MHz","250MHz" };
            static const int          clock_mhz_arr[]   = {  0,       125,     150,     200,     250 };

            enum { ACT_NONE, ACT_SAVE_STATE, ACT_LOAD_STATE, ACT_QUIT };

            char sta_path[NES_PICKER_PATH_MAX];
            make_sidecar_path(sta_path, sizeof(sta_path), name, ".sta");
            FIL _f;
            bool sta_exists = (f_open(&_f, sta_path, FA_READ) == FR_OK);
            if (sta_exists) f_close(&_f);

            nes_menu_item_t items[12];
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
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_TOGGLE,
                  .label = "Blend", .value_ptr = &v_blend,
                  .enabled = (scale_mode == SCALE_FIT) };
            items[n_items++] = (nes_menu_item_t){ .kind = NES_MENU_KIND_CHOICE,
                  .label = "Overclock", .value_ptr = &v_clock,
                  .choices = clock_choices, .num_choices = 5,
                  .enabled = true, .suffix = "next launch" };
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
                if (scale_mode == SCALE_CROP) { pan_x = 64; pan_y = 48; }
                cfg_dirty = true;
            }
            if (v_vol != volume) {
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
            if ((bool)v_ff  != fast_forward) fast_forward = (bool)v_ff;
            if ((bool)v_fps != show_fps    ) { show_fps = (bool)v_fps; cfg_dirty = true; }
            if (v_blend != blend) { blend = v_blend; cfg_dirty = true; }
            int new_mhz = clock_mhz_arr[v_clock];
            if (new_mhz != cart_clock_mhz) {
                cart_clock_mhz = new_mhz;
                cfg_dirty = true;
                if (new_mhz == 0) snprintf(osd_text, sizeof(osd_text), "clock: global");
                else              snprintf(osd_text, sizeof(osd_text), "clock %d (next)", new_mhz);
                osd_text_ms = 1500;
            }

            if (r.kind == NES_MENU_ACTION) {
                switch (r.action_id) {
                case ACT_SAVE_STATE: {
                    int rc = pcec_save_state(sta_path);
                    nes_flash_disk_flush();
                    snprintf(osd_text, sizeof(osd_text),
                              rc == 0 ? "state saved" : "save fail");
                    osd_text_ms = 1000;
                    break;
                }
                case ACT_LOAD_STATE: {
                    int rc = pcec_load_state(sta_path);
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

        /* Cart always sees input — pan controls live on MENU+dpad. */
        pcec_set_buttons(menu_down ? 0 : read_pce_buttons());

        /* Adaptive frameskip — if last frame's emulation alone blew
         * the budget, skip the scanline composer this frame. Never
         * under fast-forward (already running flat out) and never
         * more than SKIP_STREAK_MAX in a row. */
        int skip_this = 0;
        if (!fast_forward
            && (int)prev_emu_us > SKIP_BUDGET_US
            && skip_streak < SKIP_STREAK_MAX)
        {
            skip_this = 1;
            skip_streak++;
            skipped_count++;
        } else {
            skip_streak = 0;
        }
        pcec_set_skip_render(skip_this);

        /* Double-buffered render: pick the buffer NOT currently being
         * shipped to the LCD. DMA from the previous frame is reading
         * the other buffer, so the scanline composer can write this
         * one freely — no wait_idle stall before render.
         *
         * Skip frames bind NULL (no render) and ALSO don't present
         * (LCD keeps the last good frame). If we presented on skip,
         * we'd ship a stale or zero-filled buffer every other frame
         * and the screen would alternate good/garbage = flicker.
         *
         * The picker/menu/exit path expects the visible frame to live
         * in `fb`, so we copy the back buffer back when opening the
         * menu (~150 µs memcpy, off the hot path). */
        uint16_t *cur_fb = NULL;
        if (!skip_this) {
            fb_toggle++;
            cur_fb = (fb_toggle & 1) ? pce_fb_back : fb;
        }

        int frame_runs = fast_forward ? 4 : 1;
        uint32_t t_emu0 = (uint32_t)time_us_64();
        for (int i = 0; i < frame_runs; i++) {
            uint16_t *target = (i != frame_runs - 1) ? NULL : cur_fb;
            pcec_set_scale_target(target, blend);
            pcec_run_frame();
        }
        prev_emu_us = (uint32_t)time_us_64() - t_emu0;
        unsaved_play_frames += frame_runs;

        if (cur_fb) {
            if (show_fps) {
                /* fps + frames skipped in the last 1-sec window.
                 * Drawn on top of the game without a black bar. */
                char ftxt[16];
                snprintf(ftxt, sizeof(ftxt), "%2d k%2u%s",
                         fps_show, (unsigned)skipped_show,
                         fast_forward ? " FF" : "");
                nes_font_draw(cur_fb, ftxt, 2, 5, 0xFFE0);
            }
            if (osd_text_ms > 0) {
                int w = nes_font_width(osd_text);
                nes_font_draw(cur_fb, osd_text, (128 - w) / 2, 60, 0xFFE0);
                osd_text_ms -= 16;
            }
            /* wait_idle happens HERE — right before present — so emu+
             * render above runs concurrently with the previous frame's
             * DMA. In steady state this should be ~0. */
            uint32_t t_wait0 = (uint32_t)time_us_64();
            nes_lcd_wait_idle();
            prev_wait_us = (uint32_t)time_us_64() - t_wait0;
            nes_lcd_present(cur_fb);
            last_presented_fb = cur_fb;
        } else if (osd_text_ms > 0) {
            osd_text_ms -= 16;     /* still tick down the OSD timer */
        }

        {
            uint32_t t_aud0 = (uint32_t)time_us_64();
            int n = pcec_audio_pull(audio, 1024);
            if (n > 0) {
                scale_audio(audio, n, volume);
                nes_audio_pwm_push(audio, n);
            }
            prev_aud_us = (uint32_t)time_us_64() - t_aud0;
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
        /* End-to-end iteration time: from last cycle's t0 to the
         * top of THIS cycle. Sampled here AFTER the sleep_until so it
         * reflects the actual frame pacing. */
        {
            uint64_t now = (uint64_t)time_us_64();
            prev_cycle_us = (uint32_t)(now - prev_cycle_t0);
            prev_cycle_t0 = now;
        }

        if (absolute_time_diff_us(fps_window, get_absolute_time()) > 1000000) {
            fps_show = fps_frames; fps_frames = 0;
            skipped_show = skipped_count; skipped_count = 0;
            emu_us_show  = prev_emu_us;
            cycle_us_show = prev_cycle_us;
            wait_us_show  = prev_wait_us;
            aud_us_show   = prev_aud_us;
            fps_window = get_absolute_time();
        }
    }

    battery_save(name);
    if (cfg_dirty) cfg_save(name, scale_mode, show_fps, volume, blend, cart_clock_mhz);
    nes_lcd_backlight(1);
    pcec_shutdown();
    free(rom_alloc);
    while (!gpio_get(BTN_MENU_GP)) sleep_ms(10);
    return 0;
}
