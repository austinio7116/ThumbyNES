/*
 * ThumbyNES — device-side SMS / Game Gear runner.
 *
 * Mirrors nes_run.c shape but drives smsplus instead of nofrendo.
 * Reuses every hardware driver as-is (LCD, audio PWM, buttons, FAT
 * battery sidecars, idle-sleep). The only SMS/GG-specific bits are
 * the scaler and the controller bit layout.
 *
 *   SMS  256x192  → blit_fit_sms : 2:1 nearest, 16 px top/bottom letterbox
 *   GG   160x144  → blit_fit_gg  : asymmetric 5:4 / 9:8 nearest, fills screen
 *   SMS native CROP : 128x128 viewport, pannable, cart paused
 */
#include "sms_run.h"
#include "sms_core.h"
#include "nes_picker.h"
#include "nes_lcd_gc9107.h"
#include "nes_audio_pwm.h"
#include "nes_buttons.h"
#include "nes_font.h"
#include "nes_thumb.h"
#include "nes_menu.h"
#include "nes_flash_disk.h"

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
    snprintf(out, outsz, "/%s%s", base, ext);
}

static void battery_load(const char *rom_name) {
    uint8_t *ram = smsc_battery_ram();
    size_t   sz  = smsc_battery_size();
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
    uint8_t *ram = smsc_battery_ram();
    size_t   sz  = smsc_battery_size();
    if (!ram || sz == 0) return;
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".sav");
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT bw = 0;
    f_write(&f, ram, (UINT)sz, &bw);
    f_close(&f);
}

/* Translate Thumby buttons → SMS pad. A/B and LB/RB lay out the same
 * way nes_run.c does, but smsplus only has Pause (SMS) and Start (GG)
 * — bind both system inputs to the same physical buttons. */
static uint8_t read_sms_buttons(bool gg) {
    uint8_t m = 0;
    if (!gpio_get(BTN_A_GP))     m |= SMSC_BTN_BUTTON2;
    if (!gpio_get(BTN_B_GP))     m |= SMSC_BTN_BUTTON1;
    if (!gpio_get(BTN_UP_GP))    m |= SMSC_BTN_UP;
    if (!gpio_get(BTN_DOWN_GP))  m |= SMSC_BTN_DOWN;
    if (!gpio_get(BTN_LEFT_GP))  m |= SMSC_BTN_LEFT;
    if (!gpio_get(BTN_RIGHT_GP)) m |= SMSC_BTN_RIGHT;
    if (gg) {
        if (!gpio_get(BTN_RB_GP)) m |= SMSC_BTN_START;
    } else {
        if (!gpio_get(BTN_RB_GP)) m |= SMSC_BTN_PAUSE;
    }
    return m;
}

/* --- scalers ------------------------------------------------------- */

/* SMS 256x192 → 128x96 nearest, centred with 16 px top/bottom letterbox. */
static void blit_fit_sms(uint16_t *fb, const uint8_t *src,
                          const uint16_t *pal) {
    memset(fb, 0, 128 * 16 * 2);
    for (int dy = 0; dy < 96; dy++) {
        const uint8_t *srow = src + (dy * 2) * 256;
        uint16_t      *drow = fb + (16 + dy) * 128;
        for (int dx = 0; dx < 128; dx++) drow[dx] = pal[srow[dx * 2]];
    }
    memset(fb + (16 + 96) * 128, 0, 128 * 16 * 2);
}

/* SMS 256x192 → 128x96 with 2x2 box average, same letterbox. */
static void blit_blend_sms(uint16_t *fb, const uint8_t *src,
                            const uint16_t *pal) {
    memset(fb, 0, 128 * 16 * 2);
    for (int dy = 0; dy < 96; dy++) {
        const uint8_t *r0 = src + (dy * 2)     * 256;
        const uint8_t *r1 = src + (dy * 2 + 1) * 256;
        uint16_t      *drow = fb + (16 + dy) * 128;
        for (int dx = 0; dx < 128; dx++) {
            int sx = dx * 2;
            uint16_t a = pal[r0[sx]];
            uint16_t b = pal[r0[sx + 1]];
            uint16_t c = pal[r1[sx]];
            uint16_t d = pal[r1[sx + 1]];
            uint32_t rsum = ((a >> 11) & 0x1F) + ((b >> 11) & 0x1F)
                          + ((c >> 11) & 0x1F) + ((d >> 11) & 0x1F);
            uint32_t gsum = ((a >>  5) & 0x3F) + ((b >>  5) & 0x3F)
                          + ((c >>  5) & 0x3F) + ((d >>  5) & 0x3F);
            uint32_t bsum = (a & 0x1F) + (b & 0x1F) + (c & 0x1F) + (d & 0x1F);
            drow[dx] = (uint16_t)(((rsum >> 2) << 11)
                                | ((gsum >> 2) <<  5)
                                |  (bsum >> 2));
        }
    }
    memset(fb + (16 + 96) * 128, 0, 128 * 16 * 2);
}

/* SMS 256x192 → 128x128 nearest, 1.5x uniform reduction, 32 source
 * columns cropped each side so the middle 192x192 of the source fills
 * the full display with square pixels and no letterbox. Playable —
 * no pause. No BLEND variant for now: 2x2 box average only yields a
 * clean result at exactly 2:1, which this mode isn't.
 *
 *   dx in [0,128): src_x = 32 + floor(dx * 1.5) = 32 + (dx * 3) / 2
 *   dy in [0,128): src_y =       floor(dy * 1.5) =     (dy * 3) / 2
 */
static void blit_fill_sms(uint16_t *fb, const uint8_t *src,
                           const uint16_t *pal) {
    for (int dy = 0; dy < 128; dy++) {
        const uint8_t *srow = src + ((dy * 3) >> 1) * 256 + 32;
        uint16_t      *drow = fb + dy * 128;
        for (int dx = 0; dx < 128; dx++) drow[dx] = pal[srow[(dx * 3) >> 1]];
    }
}

/* SMS 256x192 1:1 crop into a 128x128 window. pan in source coords. */
static void blit_crop_sms(uint16_t *fb, const uint8_t *src,
                           const uint16_t *pal, int pan_x, int pan_y) {
    if (pan_x < 0)   pan_x = 0;
    if (pan_x > 128) pan_x = 128;   /* 256 - 128 */
    if (pan_y < 0)   pan_y = 0;
    if (pan_y > 64)  pan_y = 64;    /* 192 - 128 */
    for (int dy = 0; dy < 128; dy++) {
        const uint8_t *srow = src + (pan_y + dy) * 256 + pan_x;
        uint16_t      *drow = fb + dy * 128;
        for (int dx = 0; dx < 128; dx++) drow[dx] = pal[srow[dx]];
    }
}

/* GG 160x144 → fills the 128x128 screen with asymmetric scaling.
 *
 *   160 → 128 horizontal: 5:4 reduction.  src_x = dst_x * 5 / 4
 *   144 → 128 vertical:   9:8 reduction.  src_y = dst_y * 9 / 8
 *
 * The viewport (vx, vy) is where smsplus rendered the GG sub-rect
 * inside the 256x192 SMS bitmap, so we add it as a fixed offset. */
static void blit_fit_gg(uint16_t *fb, const uint8_t *src, const uint16_t *pal,
                         int vx, int vy) {
    for (int dy = 0; dy < 128; dy++) {
        int sy = vy + (dy * 9) / 8;
        const uint8_t *srow = src + sy * 256 + vx;
        uint16_t      *drow = fb + dy * 128;
        for (int dx = 0; dx < 128; dx++) {
            int sx = (dx * 5) / 4;
            drow[dx] = pal[srow[sx]];
        }
    }
}

/* GG 1:1 native crop into a 128x128 window of the 160x144 viewport.
 * pan in source coords, max 32 horizontal × 16 vertical. */
static void blit_crop_gg(uint16_t *fb, const uint8_t *src, const uint16_t *pal,
                          int vx, int vy, int pan_x, int pan_y) {
    if (pan_x < 0)  pan_x = 0;
    if (pan_x > 32) pan_x = 32;
    if (pan_y < 0)  pan_y = 0;
    if (pan_y > 16) pan_y = 16;
    for (int dy = 0; dy < 128; dy++) {
        const uint8_t *srow = src + (vy + pan_y + dy) * 256 + vx + pan_x;
        uint16_t      *drow = fb + dy * 128;
        for (int dx = 0; dx < 128; dx++) drow[dx] = pal[srow[dx]];
    }
}

/* --- per-ROM cfg --------------------------------------------------- */

#define CFG_MAGIC   0x534D5345u   /* 'SMSE' */

#define VOL_MIN    0
#define VOL_UNITY 15
#define VOL_MAX  30
#define VOL_DEF  15

typedef enum { SCALE_FIT = 0, SCALE_FILL = 1, SCALE_CROP = 2, SCALE_COUNT } scale_mode_t;

typedef struct {
    uint32_t magic;
    uint8_t  scale_mode;
    uint8_t  show_fps;
    uint8_t  volume;
    uint8_t  blend;
    uint8_t  reserved[4];
    uint16_t clock_mhz;       /* 0 = use global; otherwise per-cart override */
    uint16_t _pad2;
} sms_cfg_t;

static void cfg_load(const char *rom_name, scale_mode_t *scale,
                      bool *show_fps, int *volume, bool *blend, int *clock_mhz) {
    (void)scale;   /* scale_mode is intentionally NOT restored. */
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return;
    sms_cfg_t c = {0};
    UINT br = 0;
    if (f_read(&f, &c, sizeof(c), &br) == FR_OK && br >= 4
        && c.magic == CFG_MAGIC) {
        if (br >= offsetof(sms_cfg_t, clock_mhz)) {
            *show_fps = c.show_fps != 0;
            if (c.volume <= VOL_MAX) *volume = c.volume;
            *blend = c.blend != 0;
        }
        if (clock_mhz && br >= sizeof(c)) {
            *clock_mhz = c.clock_mhz;
        }
    }
    f_close(&f);
}

static void cfg_save(const char *rom_name, scale_mode_t scale,
                      bool show_fps, int volume, bool blend, int clock_mhz) {
    (void)scale;
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    sms_cfg_t c = {
        .magic = CFG_MAGIC, .scale_mode = (uint8_t)SCALE_FIT,
        .show_fps = show_fps ? 1 : 0, .volume = (uint8_t)volume,
        .blend = blend ? 1 : 0, .reserved = {0,0,0,0},
        .clock_mhz = (uint16_t)clock_mhz, ._pad2 = 0,
    };
    UINT bw = 0;
    f_write(&f, &c, sizeof(c), &bw);
    f_close(&f);
}

int sms_run_clock_override(const char *rom_name) {
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;
    sms_cfg_t c = {0};
    UINT br = 0;
    f_read(&f, &c, sizeof(c), &br);
    f_close(&f);
    if (br < sizeof(c) || c.magic != CFG_MAGIC) return 0;
    return (int)c.clock_mhz;
}

/* SMS / GG output is noticeably quieter than the GB core, so apply
 * a 4× system-specific gain on top of the user volume.
 *   volume == 0          → silence
 *   volume == VOL_UNITY  → 4.0 ×
 *   volume == VOL_MAX    → 8.0 × with hard clipping at int16 bounds
 */
static void scale_audio(int16_t *buf, int n, int volume) {
    if (volume <= 0) { for (int i = 0; i < n; i++) buf[i] = 0; return; }
    for (int i = 0; i < n; i++) {
        int32_t s = (int32_t)buf[i] * volume * 4 / VOL_UNITY;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
}

int sms_run_rom(const nes_rom_entry *e, uint16_t *fb) {
    const char *name = e->name;

    /* Reuse the picker's XIP mmap path so big SMS ROMs don't touch heap.
     * Capture the mmap return code so the failure splash tells us
     * exactly why a load failed: -32 = open, -33 = size, -34 = cluster,
     * -35 = chain not contiguous (the defragmenter target). */
    const uint8_t *rom_const = NULL;
    uint8_t       *rom_alloc = NULL;
    size_t         sz        = 0;
    int mmap_rc = nes_picker_mmap_rom(name, &rom_const, &sz);
    if (mmap_rc != 0) {
        rom_alloc = nes_picker_load_rom(name, &sz);
        if (!rom_alloc) return -30 + mmap_rc;
        rom_const = rom_alloc;
    }

    if (smsc_init(SMSC_SYS_AUTO, 22050) != 0) { free(rom_alloc); return -2; }
    if (smsc_load_rom(rom_const, sz) != 0)    { free(rom_alloc); return -3; }
    battery_load(name);

    bool gg = smsc_is_gg();
    int vx, vy, vw, vh;
    smsc_viewport(&vx, &vy, &vw, &vh);

    int          volume       = VOL_DEF;
    bool         show_fps     = false;
    bool         fast_forward = false;
    bool         blend        = true;
    scale_mode_t scale_mode   = SCALE_FIT;
    int  cart_clock_mhz = 0;
    cfg_load(name, &scale_mode, &show_fps, &volume, &blend, &cart_clock_mhz);
    /* Volume is global across all carts now — pull it from /.global. */
    volume = nes_picker_global_volume();
    bool cfg_dirty = false;

    int  osd_text_ms  = 0;
    char osd_text[24] = {0};

    const uint64_t AUTOSAVE_INTERVAL_US = 30u * 1000u * 1000u;
    uint64_t       last_autosave_us     = (uint64_t)time_us_64();
    int            unsaved_play_frames  = 0;

    const int      IDLE_SLEEP_S = 90;
    uint64_t       last_input_us = (uint64_t)time_us_64();
    bool           sleeping = false;

    const uint16_t *pal = smsc_palette_rgb565();

    int  menu_press_ms = 0;
    int  menu_was_down = 0;
    int  menu_consumed = 0;
    int  open_menu     = 0;
    bool exit_after    = false;

    /* CROP only meaningful for SMS (256x192). For GG the asymmetric
     * scale already shows the entire 160x144 frame. */
    int pan_x = 64, pan_y = 32;
    int prev_lb = 0, prev_rb = 0, prev_up = 0, prev_dn = 0;
    int prev_lt = 0, prev_rt = 0, prev_a = 0;

    int16_t audio[1024];

    const uint32_t FRAME_US = 1000000u / (uint32_t)smsc_refresh_rate();
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
                snprintf(osd_text, sizeof(osd_text),
                          rc == 0 ? "shot saved" : "shot fail");
                osd_text_ms = 800;
                menu_consumed = 1;
            }
            /* GG CROP: while MENU is held, the dpad continuously
             * pans the viewport. SMS keeps its existing
             * paused-with-bare-dpad-pan behavior below. */
            if (gg && scale_mode == SCALE_CROP) {
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
             * in-game pause menu. Replaces the old MENU-hold-to-exit
             * shortcut — Quit is now a menu item. */
            if (menu_press_ms >= 500 && !menu_consumed) {
                open_menu = 1;
                menu_consumed = 1;
            }
        } else {
            if (menu_was_down) {
                if (!menu_consumed && menu_press_ms > 0 && menu_press_ms < 300) {
                    scale_mode = (scale_mode_t)((scale_mode + 1) % SCALE_COUNT);
                    /* GG already fills the screen via the asymmetric
                     * fit blitter, so the dedicated FILL mode is SMS-
                     * only — skip it in the GG cycle. */
                    if (gg && scale_mode == SCALE_FILL) {
                        scale_mode = (scale_mode_t)((scale_mode + 1) % SCALE_COUNT);
                    }
                    cfg_dirty = true;
                    if (scale_mode == SCALE_CROP) {
                        if (gg) { pan_x = 16; pan_y = 8; }   /* GG */
                        else    { pan_x = 64; pan_y = 32; }  /* SMS */
                    }
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
            /* FILL is SMS-only (GG's FIT already fills the screen),
             * so the Display choice list has a different length on
             * each system. Map between the choice index and the
             * stable scale_mode_t enum via a small lookup. */
            static const char * const display_choices_sms[] = { "FIT", "FILL", "CROP" };
            static const char * const display_choices_gg[]  = { "FIT", "CROP" };
            static const scale_mode_t sms_idx_to_mode[] = { SCALE_FIT, SCALE_FILL, SCALE_CROP };
            static const scale_mode_t gg_idx_to_mode[]  = { SCALE_FIT, SCALE_CROP };
            const char * const *display_choices = gg ? display_choices_gg : display_choices_sms;
            const scale_mode_t  *idx_to_mode    = gg ? gg_idx_to_mode     : sms_idx_to_mode;
            int display_num = gg ? 2 : 3;

            int v_scale = 0;
            for (int i = 0; i < display_num; i++)
                if (idx_to_mode[i] == scale_mode) { v_scale = i; break; }

            int v_vol   = volume;
            int v_ff    = fast_forward ? 1 : 0;
            int v_fps   = show_fps ? 1 : 0;
            int v_blend = blend ? 1 : 0;
            int v_clock = 0;
            if (cart_clock_mhz == 125) v_clock = 1;
            if (cart_clock_mhz == 150) v_clock = 2;
            if (cart_clock_mhz == 200) v_clock = 3;
            if (cart_clock_mhz == 250) v_clock = 4;

            static const char * const clock_choices[]   = { "global","125MHz","150MHz","200MHz","250MHz" };
            static const int          clock_mhz_arr[]   = {  0,       125,     150,     200,     250 };

            enum { ACT_NONE, ACT_SAVE_STATE, ACT_LOAD_STATE, ACT_QUIT };

            char sta_path[NES_PICKER_PATH_MAX];
            make_sidecar_path(sta_path, sizeof(sta_path), name, ".sta");
            FIL _f;
            bool sta_exists = (f_open(&_f, sta_path, FA_READ) == FR_OK);
            if (sta_exists) f_close(&_f);

            nes_menu_item_t items[] = {
                { .kind = NES_MENU_KIND_ACTION, .label = "Resume",
                  .enabled = true, .action_id = ACT_NONE },
                { .kind = NES_MENU_KIND_ACTION, .label = "Save state",
                  .enabled = true,       .action_id = ACT_SAVE_STATE },
                { .kind = NES_MENU_KIND_ACTION, .label = "Load state",
                  .enabled = sta_exists, .action_id = ACT_LOAD_STATE },
                { .kind = NES_MENU_KIND_CHOICE, .label = "Display",
                  .value_ptr = &v_scale, .choices = display_choices, .num_choices = display_num,
                  .enabled = true },
                { .kind = NES_MENU_KIND_SLIDER, .label = "Volume",
                  .value_ptr = &v_vol, .min = VOL_MIN, .max = VOL_MAX,
                  .enabled = true },
                { .kind = NES_MENU_KIND_TOGGLE, .label = "Fast-fwd",
                  .value_ptr = &v_ff, .enabled = true },
                { .kind = NES_MENU_KIND_TOGGLE, .label = "Show FPS",
                  .value_ptr = &v_fps, .enabled = true },
                { .kind = NES_MENU_KIND_TOGGLE, .label = "BLEND",
                  .value_ptr = &v_blend, .enabled = !gg },
                { .kind = NES_MENU_KIND_CHOICE, .label = "Overclock",
                  .value_ptr = &v_clock, .choices = clock_choices, .num_choices = 5,
                  .enabled = true, .suffix = "next launch" },
                { .kind = NES_MENU_KIND_ACTION, .label = "Quit to picker",
                  .enabled = true, .action_id = ACT_QUIT },
            };

            char sub[28];
            strncpy(sub, name, sizeof(sub) - 1);
            sub[sizeof(sub) - 1] = 0;
            char *dot = strrchr(sub, '.');
            if (dot) *dot = 0;

            nes_menu_result_t r = nes_menu_run(fb, "PAUSED", sub,
                                                items, sizeof(items) / sizeof(items[0]));

            scale_mode_t new_scale = idx_to_mode[v_scale];
            if (new_scale != scale_mode) {
                scale_mode = new_scale;
                if (scale_mode == SCALE_CROP) {
                    if (gg) { pan_x = 16; pan_y = 8; }
                    else    { pan_x = 64; pan_y = 32; }
                }
                cfg_dirty = true;
            }
            if (v_vol   != volume       ) {
                volume = v_vol;
                nes_picker_global_set_volume(v_vol);
            }
            if ((bool)v_ff    != fast_forward) { fast_forward = (bool)v_ff;       }
            if ((bool)v_fps   != show_fps    ) { show_fps     = (bool)v_fps;   cfg_dirty = true; }
            if ((bool)v_blend != blend       ) { blend        = (bool)v_blend; cfg_dirty = true; }
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
                    int rc = smsc_save_state(sta_path);
                    nes_flash_disk_flush();
                    snprintf(osd_text, sizeof(osd_text),
                              rc == 0 ? "state saved" : "save fail");
                    osd_text_ms = 1000;
                    break;
                }
                case ACT_LOAD_STATE: {
                    int rc = smsc_load_state(sta_path);
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

        if (!gg && scale_mode == SCALE_CROP) {
            /* SMS CROP: paused, dpad pans the viewport directly. */
            smsc_set_buttons(0);
            const int STEP = 4;
            if (!menu_down) {
                if (up_down)   pan_y -= STEP;
                if (dn_down)   pan_y += STEP;
                if (lt_down)   pan_x -= STEP;
                if (rt_down)   pan_x += STEP;
                if (pan_x < 0)   pan_x = 0;
                if (pan_x > 128) pan_x = 128;
                if (pan_y < 0)   pan_y = 0;
                if (pan_y > 64)  pan_y = 64;
            }
        } else {
            /* FIT for either system, or GG CROP — cart receives input.
             * In GG CROP we additionally suppress the cart's view of
             * the dpad while MENU is held so MENU+dpad can pan. */
            smsc_set_buttons((gg && scale_mode == SCALE_CROP && menu_down)
                              ? 0
                              : read_sms_buttons(gg));
        }

        /* SMS CROP is the only path where the cart pauses; GG CROP
         * keeps ticking like FIT. SMS always boots in FIT (cfg never
         * restores scale_mode) so the buffer is always populated by
         * the time the user can toggle to CROP. */
        if (gg || scale_mode != SCALE_CROP) {
            int frame_runs = fast_forward ? 4 : 1;
            for (int i = 0; i < frame_runs; i++) smsc_run_frame();
            unsaved_play_frames += frame_runs;
        }

        const uint8_t *frame = smsc_framebuffer();
        if (frame) {
            nes_lcd_wait_idle();
            if (gg) {
                if (scale_mode == SCALE_CROP)
                    blit_crop_gg(fb, frame, pal, vx, vy, pan_x, pan_y);
                else
                    blit_fit_gg (fb, frame, pal, vx, vy);
            } else if (scale_mode == SCALE_CROP) {
                blit_crop_sms(fb, frame, pal, pan_x, pan_y);
            } else if (scale_mode == SCALE_FILL) {
                /* BLEND flag intentionally ignored — FILL is always
                 * nearest because 1.5x reduction has no clean 2x2
                 * kernel. */
                blit_fill_sms(fb, frame, pal);
            } else if (blend) {
                blit_blend_sms(fb, frame, pal);
            } else {
                blit_fit_sms(fb, frame, pal);
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

        if (gg || scale_mode != SCALE_CROP) {
            int n = smsc_audio_pull(audio, 1024);
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
    if (cfg_dirty) cfg_save(name, scale_mode, show_fps, volume, blend, cart_clock_mhz);
    nes_lcd_backlight(1);
    smsc_shutdown();
    free(rom_alloc);
    while (!gpio_get(BTN_MENU_GP)) sleep_ms(10);
    return 0;
}
