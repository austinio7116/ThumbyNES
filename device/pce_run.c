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

/* PCE natively 256×224 (most carts). The scanline renderer composites
 * directly into the bound LCD framebuffer; the core's full XBUF was
 * removed when src/pce_render.c landed. PCE_SRC_W / PCE_SRC_H below
 * are still used as the LB-pan defaults for CROP/FILL — actual source
 * dimensions come from io.screen_w / io.screen_h at frame time. */
#define PCE_SRC_W   256
#define PCE_SRC_H   224

/* Second LCD-shaped framebuffer for double-buffered DMA. The scanline
 * renderer writes directly to whichever buffer is not currently being
 * shipped to the GC9107, so emu+render of frame N overlaps the DMA of
 * frame N-1. Without this, every frame stalls ~10 ms on
 * nes_lcd_wait_idle before it can touch fb.
 *
 * malloc'd at the top of pce_run_rom and freed on exit so the 32 KB
 * goes back to the heap whenever the user is in another emulator
 * slot (NES/SMS/GB/MD) or running the picker / defragment. Without
 * this, ThumbyOne builds with PCE compiled in are left with so little
 * heap (~129 KB) that MD / SMS / GB ROM loaders + the defragment
 * planner all fail. */
static uint16_t *pce_fb_back = NULL;

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
 * when held. Shoulders map:
 *   LB → SELECT  (doubles as the CROP-pan modifier — main loop
 *                 strips SELECT while LB is held and re-asserts it
 *                 as a release pulse only if no pan happened, so
 *                 a tap still acts as SELECT and the chord never
 *                 leaks SELECT to the cart mid-pan).
 *   RB → RUN
 * Same release-pulse pattern md_run.c uses for LB → START. */
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

/* --- scaling ------------------------------------------------------- *
 *
 * The scanline renderer (src/pce_render.c) does the FIT/FILL/CROP
 * conversion directly into the 128×128 LCD framebuffer. The runner's
 * job is to forward the menu's scale_mode + the LB-pan offsets to
 * pcec_set_scale_target each frame; no per-row blit lives here.
 *
 *   FIT  — 2:1 nearest/blend, letterboxed Y.
 *   FILL — preserve aspect via Y scale → square output, src cols
 *          cropped, pannable horizontally with LB+L/R.
 *   CROP — 1:1 native 128×128 window into src (256×224 typical),
 *          pannable both axes with LB+d-pad. Same chord as md_run.c.
 */

/* Centred FILL pan_x = (vw - visible_w) / 2 where visible_w =
 * min(vh, vw). For 256×224 cards this is 16 (the common case);
 * for 256×240 carts (R-Type) it's 8; for wider 336× modes it's
 * 56/48. Hard-coding 16 was right for 224-line carts only and
 * pinned wider/taller modes against the right edge. */
static int pce_fill_pan_x_centre(void) {
    int vx, vy, vw, vh;
    pcec_viewport(&vx, &vy, &vw, &vh);
    if (vh <= 0 || vw <= 0) return 0;
    int visible_w = vh;
    if (visible_w > vw) visible_w = vw;
    int max_pan = vw - visible_w; if (max_pan < 0) max_pan = 0;
    return max_pan / 2;
}

/* --- per-ROM cfg --------------------------------------------------- */

#define CFG_MAGIC   0x50434558u   /* 'PCEX' */

#define VOL_MIN    0
#define VOL_UNITY 15
#define VOL_MAX  30
#define VOL_DEF  15

/* Mirror md_run.c numbering so pce_render.c sees the same encoding:
 *   0 = FIT (letterboxed 2:1), 1 = FILL (aspect-preserving),
 *   2 = CROP (1:1 native window). */
typedef enum { SCALE_FIT = 0, SCALE_FILL = 1, SCALE_CROP = 2,
                SCALE_COUNT } scale_mode_t;

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

    /* Allocate the back framebuffer first — it's 32 KB and we need it
     * for the double-buffered DMA path. Live for the duration of the
     * session, freed on exit so other emulators / the picker get the
     * 32 KB back. */
    if (!pce_fb_back) {
        pce_fb_back = (uint16_t *)malloc(128 * 128 * sizeof(uint16_t));
        if (!pce_fb_back) return -40;     /* surfaces as load err -40 */
    }
    /* malloc is uninitialised — clear before first present so the user
     * doesn't see one frame of stale heap when ping-ponging buffers. */
    memset(pce_fb_back, 0, 128 * 128 * sizeof(uint16_t));

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
        if (!rom_alloc) {
            free(pce_fb_back); pce_fb_back = NULL;
            return -30 + mmap_rc;
        }
        rom_const = rom_alloc;
    }

    /* US-encoded (bit-reversed) HuCards aren't supported on the device.
     * Pre-decoded dumps work fine and are the de-facto standard in
     * modern PCE/TG-16 dump sets, so we just refuse to load encrypted
     * blobs and surface a load-error in the picker. */
    if (pcec_rom_is_us_encoded(rom_const, sz)) {
        free(rom_alloc);
        free(pce_fb_back); pce_fb_back = NULL;
        return -22;     /* surfaces as "load err -22" in nes_device_main */
    }

    if (pcec_init(PCEC_REGION_AUTO, 22050) != 0) {
        free(rom_alloc);
        free(pce_fb_back); pce_fb_back = NULL;
        return -20;
    }
    if (pcec_load_rom(rom_const, sz) != 0) {
        free(rom_alloc);
        pcec_shutdown();
        free(pce_fb_back); pce_fb_back = NULL;
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

    /* Pan offsets in source pixels. Defaults centre the source window:
     *   CROP: (256-128)/2 = 64,  (224-128)/2 = 48
     *   FILL: (256-224)/2 = 16,  unused y (FILL doesn't pan vertically)
     * Reset on mode change so a switch into FILL doesn't pin pan_x at
     * 64 (FILL clamps to [0, 32], so 64 would be visually right-pinned). */
    int pan_x = 64, pan_y = 48;
    int prev_a = 0;

    /* LB chord for play-while-cropped panning — mirrors md_run.c.
     * While LB is held in CROP mode the d-pad pans the source
     * viewport instead of going to the cart, and LB→SELECT is
     * suppressed so SELECT doesn't fire mid-pan. On LB release: if
     * no pan motion happened during the hold, pulse SELECT to the
     * cart for a few frames so a "tap LB" still acts as SELECT. */
    int  lb_held_frames  = 0;
    bool lb_pan_used     = false;
    int  lb_select_pulse = 0;

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
        int lb_down   = !gpio_get(BTN_LB_GP);
        int up_down   = !gpio_get(BTN_UP_GP);
        int dn_down   = !gpio_get(BTN_DOWN_GP);
        int lt_down   = !gpio_get(BTN_LEFT_GP);
        int rt_down   = !gpio_get(BTN_RIGHT_GP);
        int a_down    = !gpio_get(BTN_A_GP);
        int any_input = menu_down || lb_down || up_down || dn_down
                        || lt_down || rt_down
                        || a_down || !gpio_get(BTN_B_GP)
                        || !gpio_get(BTN_RB_GP);

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

        /* CROP pan: hold LB to engage. Same chord as md_run.c —
         * d-pad pans the viewport (cart sees no d-pad and LB→SELECT
         * is suppressed). Default — without LB held — d-pad + LB go
         * to the cart, so the game stays fully playable while
         * cropped. On LB release: pulse SELECT only if no pan
         * motion happened during the hold (a "tap LB" still acts as
         * SELECT); if the chord was used, swallow SELECT to avoid
         * accidentally firing on chord exit. */
        if (lb_down) {
            lb_held_frames++;
            const int PAN_STEP = 2;
            int dx = (rt_down ? PAN_STEP : 0) - (lt_down ? PAN_STEP : 0);
            int dy = (dn_down ? PAN_STEP : 0) - (up_down ? PAN_STEP : 0);
            if (scale_mode == SCALE_CROP && (dx || dy)) {
                /* CROP — full pan range (256-128, 224-128). */
                pan_x += dx;
                pan_y += dy;
                if (pan_x < 0)   pan_x = 0;
                if (pan_x > 128) pan_x = 128;
                if (pan_y < 0)   pan_y = 0;
                if (pan_y > 96)  pan_y = 96;
                lb_pan_used = true;
            } else if (scale_mode == SCALE_FILL && dx) {
                /* FILL — horizontal only. Visible window is 224 cols
                 * (= act_h) inside 256-wide src, so pan_x has a tight
                 * [0, 32] range. dy ignored. */
                pan_x += dx;
                if (pan_x < 0)  pan_x = 0;
                if (pan_x > 32) pan_x = 32;
                lb_pan_used = true;
            }
        } else {
            if (lb_held_frames > 0 && !lb_pan_used) {
                /* Pulse SELECT for ~3 frames so the cart definitely
                 * registers the press, regardless of when in its tick
                 * cycle the read lands. */
                lb_select_pulse = 3;
            }
            lb_held_frames = 0;
            lb_pan_used    = false;
        }
        if (lb_select_pulse > 0) lb_select_pulse--;

        if (menu_down) {
            menu_press_ms += 16;
            menu_was_down = 1;
            /* MENU+A: framebuffer snapshot. */
            if (a_down && !prev_a) {
                int rc = nes_thumb_save(fb, name);
                /* Force the .scr32/.scr64 writes through the FAT
                 * write-cache to flash. Without this, a power-off
                 * before the next battery_save / cfg_save / shutdown
                 * loses the screenshot. */
                if (rc == 0) nes_flash_disk_flush();
                snprintf(osd_text, sizeof(osd_text),
                          rc == 0 ? "shot saved" : "shot fail");
                osd_text_ms = 800;
                menu_consumed = 1;
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
                    /* Recentre pan on mode change — each mode has a
                     * different legal range. Stale pan_x=64 in FILL
                     * would clamp to 32 → permanently right-pinned. */
                    if      (scale_mode == SCALE_CROP) { pan_x = 64; pan_y = 48; }
                    else if (scale_mode == SCALE_FILL) { pan_x = pce_fill_pan_x_centre(); pan_y = 0; }
                    else                               { pan_x = 0;  pan_y = 0;  }
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

            static const char * const display_choices[] = { "FIT", "FILL", "CROP" };
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
                  .choices = display_choices, .num_choices = 3, .enabled = true };
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
                  .enabled = (scale_mode != SCALE_CROP) };
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
                if      (scale_mode == SCALE_CROP) { pan_x = 64; pan_y = 48; }
                else if (scale_mode == SCALE_FILL) { pan_x = 16; pan_y = 0;  }
                else                               { pan_x = 0;  pan_y = 0;  }
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

        /* Apply LB chord. While LB is held in CROP the d-pad pans the
         * viewport — strip direction bits + LB→SELECT before the cart
         * sees them. On LB release without pan, replay SELECT as a
         * 3-frame pulse so a "tap LB" still acts as SELECT. MENU held
         * masks all input so the long-hold-to-open-menu and MENU+A
         * screenshot chord don't leak into the cart. */
        uint16_t pad = read_pce_buttons();
        if (menu_down) {
            pad = 0;
        } else {
            if (lb_down) {
                pad &= ~PCEC_BTN_SELECT;
                if (scale_mode == SCALE_CROP) {
                    pad &= ~(PCEC_BTN_UP | PCEC_BTN_DOWN
                             | PCEC_BTN_LEFT | PCEC_BTN_RIGHT);
                }
            }
            if (lb_select_pulse > 0) {
                pad |= PCEC_BTN_SELECT;
            }
        }
        pcec_set_buttons(pad);

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
            pcec_set_scale_target(target, (int)scale_mode, blend, pan_x, pan_y);
            pcec_run_frame();
        }
        prev_emu_us = (uint32_t)time_us_64() - t_emu0;
        unsaved_play_frames += frame_runs;

        if (cur_fb) {
            if (show_fps) {
                /* fps, with " FF" when fast-forwarding and " k<N>"
                 * only when frames were skipped in the last window. */
                char ftxt[16];
                if (skipped_show > 0) {
                    snprintf(ftxt, sizeof(ftxt), "%d%s k%u",
                             fps_show,
                             fast_forward ? " FF" : "",
                             (unsigned)skipped_show);
                } else {
                    snprintf(ftxt, sizeof(ftxt), "%d%s",
                             fps_show,
                             fast_forward ? " FF" : "");
                }
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
    free(pce_fb_back); pce_fb_back = NULL;     /* 32 KB back to heap */
    while (!gpio_get(BTN_MENU_GP)) sleep_ms(10);
    return 0;
}
