/*
 * ThumbyNES — device ROM runner.
 *
 * Bridges the cross-platform Nofrendo wrapper (src/nes_core.[ch])
 * to the Thumby Color hardware drivers. Per frame:
 *
 *   1. Read buttons → NES controller mask → nesc_set_buttons()
 *   2. nesc_run_frame()
 *   3. Downscale 256×240 palette-indexed → 128×120 RGB565 with
 *      a 4 px letterbox top/bottom (Option A from PLAN.md §4).
 *   4. nes_lcd_present() the 128×128 framebuffer.
 *   5. nesc_audio_pull() → nes_audio_pwm_push() (22050 Hz mono).
 *
 * MENU long-press exits back to the picker.
 */
#include "nes_run.h"
#include "nes_core.h"
#include "nes_picker.h"
#include "nes_lcd_gc9107.h"
#include "nes_audio_pwm.h"
#include "nes_buttons.h"

#include <stddef.h>   /* offsetof */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "ff.h"

#include "nes_font.h"
#include "nes_thumb.h"
#include "nes_menu.h"
#include "nes_flash_disk.h"

/* Pin map mirrors nes_buttons.c. We read raw GPIOs here so we can
 * remap LB/RB to Select/Start without going through the PICO-8
 * LRUDOX layout that nes_buttons_read() returns. */
#define BTN_LEFT_GP   0
#define BTN_UP_GP     1
#define BTN_RIGHT_GP  2
#define BTN_DOWN_GP   3
#define BTN_LB_GP     6
#define BTN_A_GP     21
#define BTN_RB_GP    22
#define BTN_B_GP     25
#define BTN_MENU_GP  26

/* Build a per-ROM sidecar path. Strips whatever extension the ROM
 * had and appends `ext` so `Zelda.nes` → `/Zelda.sav`, `/Zelda.cfg`. */
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
    uint8_t *ram = nesc_battery_ram();
    size_t   sz  = nesc_battery_size();
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
    uint8_t *ram = nesc_battery_ram();
    size_t   sz  = nesc_battery_size();
    if (!ram || sz == 0) return;

    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".sav");

    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT bw = 0;
    f_write(&f, ram, (UINT)sz, &bw);
    f_close(&f);
}

static uint8_t read_nes_buttons(void) {
    uint8_t m = 0;
    if (!gpio_get(BTN_A_GP))     m |= NESC_BTN_A;
    if (!gpio_get(BTN_B_GP))     m |= NESC_BTN_B;
    if (!gpio_get(BTN_LB_GP))    m |= NESC_BTN_SELECT;
    if (!gpio_get(BTN_RB_GP))    m |= NESC_BTN_START;
    if (!gpio_get(BTN_UP_GP))    m |= NESC_BTN_UP;
    if (!gpio_get(BTN_DOWN_GP))  m |= NESC_BTN_DOWN;
    if (!gpio_get(BTN_LEFT_GP))  m |= NESC_BTN_LEFT;
    if (!gpio_get(BTN_RIGHT_GP)) m |= NESC_BTN_RIGHT;
    return m;
}

/* Scaling modes:
 *   FIT  — 256×240 → 128×120 downscale, centred with 4 px letterbox
 *          top + bottom. Either nearest-neighbor (drop) or 2×2 box
 *          average depending on the orthogonal `blend` toggle.
 *   CROP — 1:1 native pixels, 128×128 viewport into the 256×240
 *          frame, pannable with the D-pad. Pauses emulation. */
typedef enum {
    SCALE_FIT  = 0,
    SCALE_CROP = 1,
    SCALE_COUNT
} scale_mode_t;

/* 256×240 → 128×120 nearest-neighbor downscale, centered with 4 px
 * black letterbox. Source is 8-bit palette indices, NES_SCREEN_PITCH
 * stride, 8 px of overdraw on the left edge. */
static void blit_fit(uint16_t *fb, const uint8_t *src, int pitch,
                      const uint16_t *pal) {
    memset(fb, 0, 128 * 4 * 2);
    for (int dy = 0; dy < 120; dy++) {
        const uint8_t *srow = src + (dy * 2) * pitch + 8 /* overdraw */;
        uint16_t      *drow = fb + (4 + dy) * 128;
        for (int dx = 0; dx < 128; dx++) {
            drow[dx] = pal[srow[dx * 2]];
        }
    }
    memset(fb + (4 + 120) * 128, 0, 128 * 4 * 2);
}

/* 256×240 → 128×120 with a 2×2 box average per output pixel. Same
 * letterbox as blit_fit. For each destination pixel we read four
 * source palette indices, look up RGB565 for each, then average the
 * R/G/B channels independently in their packed bit positions. ~16
 * ops per pixel × 15360 pixels × 60 fps ≈ 15 M ops/sec — easy. */
static void blit_blend(uint16_t *fb, const uint8_t *src, int pitch,
                        const uint16_t *pal) {
    memset(fb, 0, 128 * 4 * 2);
    for (int dy = 0; dy < 120; dy++) {
        const uint8_t *r0 = src + (dy * 2)     * pitch + 8 /* overdraw */;
        const uint8_t *r1 = src + (dy * 2 + 1) * pitch + 8;
        uint16_t      *drow = fb + (4 + dy) * 128;
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
            /* Divide each channel by 4 and repack. */
            drow[dx] = (uint16_t)(((rsum >> 2) << 11)
                                | ((gsum >> 2) <<  5)
                                |  (bsum >> 2));
        }
    }
    memset(fb + (4 + 120) * 128, 0, 128 * 4 * 2);
}

/* 1:1 native crop. Copies a 128×128 window starting at NES (pan_x,
 * pan_y), clamped to the 256×240 frame (so pan_x ∈ [0, 128],
 * pan_y ∈ [0, 112]). When pan = (64, 56) the viewport is centred. */
static void blit_crop(uint16_t *fb, const uint8_t *src, int pitch,
                       const uint16_t *pal, int pan_x, int pan_y) {
    if (pan_x < 0)   pan_x = 0;
    if (pan_x > 128) pan_x = 128;
    if (pan_y < 0)   pan_y = 0;
    if (pan_y > 112) pan_y = 112;
    for (int dy = 0; dy < 128; dy++) {
        const uint8_t *srow = src + (pan_y + dy) * pitch + 8 /* overdraw */ + pan_x;
        uint16_t      *drow = fb + dy * 128;
        for (int dx = 0; dx < 128; dx++) {
            drow[dx] = pal[srow[dx]];
        }
    }
}

/* --- persisted config ---------------------------------------------- */

/* Per-ROM sidecar (`<rom>.cfg`): a tiny versioned struct. Each game
 * remembers its own scale mode, palette, FPS overlay, volume, and
 * blend toggle. Magic is bumped whenever the layout or the meaning
 * of any field changes; older files are silently treated as defaults. */
#define CFG_MAGIC   0x4E455345u   /* 'NESE' = NES cfg v3 */

#define VOL_MIN    0
#define VOL_UNITY 15
#define VOL_MAX  30
#define VOL_DEF  15

typedef struct {
    uint32_t magic;
    uint8_t  scale_mode;   /* 0 = FIT, 1 = CROP */
    uint8_t  show_fps;
    uint8_t  palette;
    uint8_t  volume;       /* legacy — global volume in /.global wins */
    uint8_t  blend;        /* 0 / 1 — orthogonal to scale_mode */
    uint8_t  pal;          /* 0 = NTSC (60 Hz), 1 = PAL (50 Hz) */
    uint8_t  reserved[2];
    uint16_t clock_mhz;    /* 0 = use global; otherwise 125/150/200/250 */
    uint16_t _pad2;
} nes_cfg_t;

static void cfg_load(const char *rom_name, scale_mode_t *scale,
                      bool *show_fps, int *palette, int *volume,
                      bool *blend, bool *pal, int *clock_mhz) {
    (void)scale;   /* scale_mode is intentionally NOT restored — every
                    * session boots in FIT and the user toggles to CROP
                    * by hand if they want it. Avoids the black-screen
                    * trap when a stale CROP cfg races the cart's first
                    * rendered frame. */
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return;
    nes_cfg_t c = {0};       /* unread fields stay at their defaults */
    UINT br = 0;
    if (f_read(&f, &c, sizeof(c), &br) == FR_OK && br >= 4
        && c.magic == CFG_MAGIC) {
        if (br >= offsetof(nes_cfg_t, clock_mhz)) {
            *show_fps = c.show_fps != 0;
            if (c.palette < NESC_PALETTE_COUNT)  *palette = c.palette;
            if (c.volume <= VOL_MAX)             *volume  = c.volume;
            *blend = c.blend != 0;
            *pal   = c.pal != 0;
        }
        /* clock_mhz appended in a later cfg version — only honor it
         * if the file is long enough to actually contain the field. */
        if (clock_mhz && br >= sizeof(c)) {
            *clock_mhz = c.clock_mhz;
        }
    }
    f_close(&f);
}

static void cfg_save(const char *rom_name, scale_mode_t scale,
                      bool show_fps, int palette, int volume,
                      bool blend, bool pal, int clock_mhz) {
    (void)scale;   /* always written as FIT — see cfg_load comment. */
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    nes_cfg_t c = {
        .magic      = CFG_MAGIC,
        .scale_mode = (uint8_t)SCALE_FIT,
        .show_fps   = show_fps ? 1 : 0,
        .palette    = (uint8_t)palette,
        .volume     = (uint8_t)volume,
        .blend      = blend ? 1 : 0,
        .pal        = pal ? 1 : 0,
        .reserved   = {0, 0},
        .clock_mhz  = (uint16_t)clock_mhz,
        ._pad2      = 0,
    };
    UINT bw = 0;
    f_write(&f, &c, sizeof(c), &bw);
    f_close(&f);
}

int nes_run_clock_override(const char *rom_name) {
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;
    nes_cfg_t c = {0};
    UINT br = 0;
    f_read(&f, &c, sizeof(c), &br);
    f_close(&f);
    if (br < sizeof(c) || c.magic != CFG_MAGIC) return 0;
    return (int)c.clock_mhz;
}

/* Linear volume scaling around VOL_UNITY = 15.
 *   volume == 0          → silence
 *   volume == VOL_UNITY  → unity passthrough (1.0 ×)
 *   volume == VOL_MAX    → 2.0 × with hard clipping
 * The cores' raw output sits well below ±32767 so the 2x ceiling
 * has plenty of headroom on most carts before clipping kicks in. */
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

int nes_run_rom(const nes_rom_entry *e, uint16_t *fb) {
    const char *name = e->name;
    /* Try the zero-copy XIP mmap path first. Works for any ROM
     * that lives contiguously on the flash disk (which is every
     * file written to a fresh volume). Falls back to a malloc'd
     * RAM copy if the file is fragmented — only viable for small
     * ROMs given the ~340 KB free heap budget. */
    const uint8_t *rom_const = NULL;
    uint8_t       *rom_alloc = NULL;
    size_t         sz        = 0;

    int mmap_rc = nes_picker_mmap_rom(name, &rom_const, &sz);
    if (mmap_rc != 0) {
        rom_alloc = nes_picker_load_rom(name, &sz);
        /* Encode the mmap return code so the splash tells us exactly
         * why the load failed: -32 = f_open, -33 = size out of range,
         * -34 = bad start cluster, -35 = chain not contiguous. */
        if (!rom_alloc) return -30 + mmap_rc;
        rom_const = rom_alloc;
    }

    /* We need cfg before nesc_init to know the region. Peek the
     * pal flag from the cfg file if it exists, defaulting to the
     * picker's auto-detect hint when no cfg is present. The full
     * cfg load happens again below — this is just to pick the
     * region for nesc_init. */
    {
        scale_mode_t _s = SCALE_FIT;
        int _p = 0, _v = VOL_DEF;
        bool _f = false, _b = true;
        bool _pal = (e->pal_hint != 0);
        int _clk = 0;
        cfg_load(name, &_s, &_f, &_p, &_v, &_b, &_pal, &_clk);
        if (nesc_init(_pal ? NESC_SYS_PAL : NESC_SYS_NTSC, 22050) != 0)
            { free(rom_alloc); return -2; }
    }
    if (nesc_load_rom(rom_const, sz) != 0)         { free(rom_alloc); return -3; }

    /* Restore the battery save (if any) before the cart starts running. */
    battery_load(name);

    /* Defaults: FIT with BLEND on, fast-forward off, FPS hidden,
     * COMPOSITE palette, comfortable mid-volume. Region defaults
     * come from the picker's auto-detect (iNES header + filename
     * heuristic). Per-ROM /<name>.cfg overrides any of these. */
    int          palette       = 1;   /* COMPOSITE — warmer than NOFRENDO */
    int          volume        = VOL_DEF;
    bool         show_fps      = false;
    bool         fast_forward  = false;
    bool         blend         = true;
    bool         pal_mode      = (e->pal_hint != 0);
    scale_mode_t scale_mode    = SCALE_FIT;

    int  cart_clock_mhz = 0;   /* 0 = use global; otherwise per-cart override */
    cfg_load(name, &scale_mode, &show_fps, &palette, &volume, &blend, &pal_mode, &cart_clock_mhz);
    /* Volume is global across all carts now — pull it from /.global. */
    volume = nes_picker_global_volume();
    nesc_set_palette(palette);
    bool cfg_dirty = false;

    /* Volume / brightness OSD: shows for ~1 s after a change. */
    int  osd_text_ms  = 0;
    char osd_text[24] = {0};

    /* Auto-save battery: triggered every AUTOSAVE_INTERVAL of wall
     * time when there have been gameplay frames since the last save.
     * Cheap insurance against power loss. */
    const uint64_t AUTOSAVE_INTERVAL_US = 30u * 1000u * 1000u;
    uint64_t       last_autosave_us     = (uint64_t)time_us_64();
    int            unsaved_play_frames  = 0;

    /* Idle sleep: dim + halt after IDLE_SLEEP_S of no input. */
    const int      IDLE_SLEEP_S = 90;
    uint64_t       last_input_us = (uint64_t)time_us_64();
    bool           sleeping = false;

    const uint16_t *pal   = nesc_palette_rgb565();
    int             pitch = nesc_framebuffer_pitch();

    /* MENU gestures:
     *   tap (< 300 ms, no chord)  → toggle FIT ↔ CROP
     *   hold (≥ 500 ms, no chord) → open in-game menu
     *   MENU + A                   → save screenshot
     * Everything else lives in the menu (palette, FPS, BLEND, region,
     * volume, fast-forward, save / load state, quit). */
    int  menu_press_ms = 0;
    int  menu_was_down = 0;
    int  menu_consumed = 0;
    int  open_menu     = 0;
    bool exit_after    = false;

    /* Pan position for CROP mode. Reset to centre on every entry. */
    int pan_x = 64;
    int pan_y = 56;
    /* Edge-detect chord buttons so a held press fires once. */
    int prev_lb = 0, prev_rb = 0, prev_up = 0, prev_dn = 0;
    int prev_lt = 0, prev_rt = 0, prev_b = 0, prev_a = 0;

    /* Per-frame audio scratch. 22050 / 60 ≈ 368 samples per frame. */
    int16_t audio[1024];

    /* Frame pacing + FPS counter. Cap at the cart's native refresh
     * rate (60 NTSC, 50 PAL) so it runs at original-hardware speed
     * — without this the 250 MHz RP2350 runs ahead. */
    const uint32_t FRAME_US = 1000000u / (uint32_t)nesc_refresh_rate();
    absolute_time_t next_frame = get_absolute_time();
    absolute_time_t fps_window = get_absolute_time();
    int fps_frames = 0;
    int fps_show   = 0;

    while (!exit_after) {
        /* MENU edge / tap / hold / chord detection.
         * NTSC frame ≈ 16 ms — we use that as the unit. */
        int menu_down = !gpio_get(BTN_MENU_GP);
        int lb_down = !gpio_get(BTN_LB_GP);
        int rb_down = !gpio_get(BTN_RB_GP);
        int up_down = !gpio_get(BTN_UP_GP);
        int dn_down = !gpio_get(BTN_DOWN_GP);
        int lt_down = !gpio_get(BTN_LEFT_GP);
        int rt_down = !gpio_get(BTN_RIGHT_GP);
        int b_down  = !gpio_get(BTN_B_GP);
        int a_down  = !gpio_get(BTN_A_GP);
        int any_input = menu_down || lb_down || rb_down || up_down || dn_down
                        || lt_down || rt_down || b_down || a_down;

        /* --- idle sleep tracking --- */
        if (any_input) {
            last_input_us = (uint64_t)time_us_64();
            if (sleeping) {
                /* Wake on any press. Re-anchor the frame-pacing
                 * deadline to "now" — otherwise next_frame is still
                 * the timestamp from however many seconds ago we
                 * last drew, sleep_until returns immediately, and
                 * the loop runs flat out at uncapped speed until
                 * wall-clock catches up. */
                sleeping = false;
                nes_lcd_backlight(1);
                next_frame = get_absolute_time();
            }
        }
        if (!sleeping &&
            (uint64_t)time_us_64() - last_input_us > (uint64_t)IDLE_SLEEP_S * 1000000u) {
            /* Persist state before going dark in case the user
             * never wakes the device back up. */
            battery_save(name);
            unsaved_play_frames = 0;
            sleeping = true;
            nes_lcd_backlight(0);
        }
        if (sleeping) {
            sleep_ms(50);
            continue;
        }

        if (menu_down) {
            menu_press_ms += 16;
            menu_was_down = 1;
            /* MENU+A: snapshot the current 128×128 framebuffer to a
             * .scr32 + .scr64 sidecar that the picker reads back as
             * inline / hero thumbnails. */
            if (a_down && !prev_a) {
                int rc = nes_thumb_save(fb, name);
                snprintf(osd_text, sizeof(osd_text),
                          rc == 0 ? "shot saved" : "shot fail");
                osd_text_ms = 800;
                menu_consumed = 1;
            }
            /* MENU long hold (≥ 500 ms with no chord) opens the
             * in-game pause menu. Fire once at the threshold and
             * latch menu_consumed so the release doesn't also fire
             * the short-tap CROP toggle. */
            if (menu_press_ms >= 500 && !menu_consumed) {
                open_menu = 1;
                menu_consumed = 1;
            }
        } else {
            if (menu_was_down) {
                /* Release. Tap (< 300 ms, no chord) = toggle scale
                 * mode. Long-hold paths already fired at the
                 * threshold and set menu_consumed. */
                if (!menu_consumed && menu_press_ms > 0 && menu_press_ms < 300) {
                    scale_mode = (scale_mode_t)((scale_mode + 1) % SCALE_COUNT);
                    cfg_dirty = true;
                    if (scale_mode == SCALE_CROP) {
                        pan_x = 64;   /* recentre on entry */
                        pan_y = 56;
                    }
                }
                menu_press_ms = 0;
                menu_was_down = 0;
                menu_consumed = 0;
            }
        }
        prev_lb = lb_down;
        prev_rb = rb_down;
        prev_up = up_down;
        prev_dn = dn_down;
        prev_lt = lt_down;
        prev_rt = rt_down;
        prev_b  = b_down;
        prev_a  = a_down;

        /* ----- in-game menu ----- */
        if (open_menu) {
            open_menu = 0;
            /* Pack current state into ints the menu can mutate. */
            int v_scale = (int)scale_mode;
            int v_vol   = volume;
            int v_ff    = fast_forward ? 1 : 0;
            int v_fps   = show_fps ? 1 : 0;
            int v_blend = blend ? 1 : 0;
            int v_pal   = palette;
            int v_pal_mode = pal_mode ? 1 : 0;
            /* The Overclock choice covers global + 4 explicit MHz
             * values. Index 0 means "use global", which falls back
             * to the system-wide default. */
            int active_clock = cart_clock_mhz ? cart_clock_mhz
                                              : nes_picker_global_clock_mhz();
            int v_clock = 0;   /* default to "global" */
            if (cart_clock_mhz == 125) v_clock = 1;
            if (cart_clock_mhz == 150) v_clock = 2;
            if (cart_clock_mhz == 200) v_clock = 3;
            if (cart_clock_mhz == 250) v_clock = 4;

            static const char * const display_choices[] = { "FIT", "CROP" };
            static const char * const clock_choices[]   = { "global","125MHz","150MHz","200MHz","250MHz" };
            static const int          clock_mhz_arr[]   = {  0,       125,     150,     200,     250 };
            (void)active_clock;
            static const char * const palette_names_arr[] = {
                "NOFRENDO", "COMPOSITE", "NESCLASS", "NTSC", "PVM", "SMOOTH",
            };
            static const char * const region_choices[] = { "NTSC", "PAL" };

            enum { ACT_NONE, ACT_SAVE_STATE, ACT_LOAD_STATE, ACT_QUIT };

            nes_menu_item_t items[] = {
                { .kind = NES_MENU_KIND_ACTION, .label = "Resume",
                  .enabled = true, .action_id = ACT_NONE },
                { .kind = NES_MENU_KIND_ACTION, .label = "Save state",
                  .enabled = true,  .action_id = ACT_SAVE_STATE },
                { .kind = NES_MENU_KIND_ACTION, .label = "Load state",
                  .enabled = ({
                      char _p[NES_PICKER_PATH_MAX];
                      make_sidecar_path(_p, sizeof(_p), name, ".sta");
                      FIL _f;
                      bool _exists = (f_open(&_f, _p, FA_READ) == FR_OK);
                      if (_exists) f_close(&_f);
                      _exists;
                  }),
                  .action_id = ACT_LOAD_STATE },
                { .kind = NES_MENU_KIND_CHOICE, .label = "Display",
                  .value_ptr = &v_scale, .choices = display_choices, .num_choices = 2,
                  .enabled = true },
                { .kind = NES_MENU_KIND_SLIDER, .label = "Volume",
                  .value_ptr = &v_vol, .min = VOL_MIN, .max = VOL_MAX,
                  .enabled = true },
                { .kind = NES_MENU_KIND_TOGGLE, .label = "Fast-fwd",
                  .value_ptr = &v_ff, .enabled = true },
                { .kind = NES_MENU_KIND_TOGGLE, .label = "Show FPS",
                  .value_ptr = &v_fps, .enabled = true },
                { .kind = NES_MENU_KIND_TOGGLE, .label = "BLEND",
                  .value_ptr = &v_blend, .enabled = true },
                { .kind = NES_MENU_KIND_CHOICE, .label = "Palette",
                  .value_ptr = &v_pal, .choices = palette_names_arr,
                  .num_choices = NESC_PALETTE_COUNT, .enabled = true },
                { .kind = NES_MENU_KIND_CHOICE, .label = "Region",
                  .value_ptr = &v_pal_mode, .choices = region_choices, .num_choices = 2,
                  .enabled = true, .suffix = "next launch" },
                { .kind = NES_MENU_KIND_CHOICE, .label = "Overclock",
                  .value_ptr = &v_clock, .choices = clock_choices, .num_choices = 5,
                  .enabled = true, .suffix = "next launch" },
                { .kind = NES_MENU_KIND_ACTION, .label = "Quit to picker",
                  .enabled = true, .action_id = ACT_QUIT },
            };

            /* Build a short cart subtitle from the ROM name. */
            char sub[28];
            strncpy(sub, name, sizeof(sub) - 1);
            sub[sizeof(sub) - 1] = 0;
            char *dot = strrchr(sub, '.');
            if (dot) *dot = 0;

            nes_menu_result_t r = nes_menu_run(fb, "PAUSED", sub,
                                                items, sizeof(items) / sizeof(items[0]));

            /* Apply value changes. */
            if (v_scale != (int)scale_mode) {
                scale_mode = (scale_mode_t)v_scale;
                if (scale_mode == SCALE_CROP) { pan_x = 64; pan_y = 56; }
                cfg_dirty = true;
            }
            if (v_vol   != volume       ) {
                volume = v_vol;
                nes_picker_global_set_volume(v_vol);
            }
            if ((bool)v_ff    != fast_forward) {  fast_forward = (bool)v_ff;          /* not persisted */ }
            if ((bool)v_fps   != show_fps    ) {  show_fps     = (bool)v_fps; cfg_dirty = true; }
            if ((bool)v_blend != blend       ) {  blend        = (bool)v_blend; cfg_dirty = true; }
            if (v_pal != palette) {
                palette = v_pal;
                nesc_set_palette(palette);
                cfg_dirty = true;
            }
            if ((bool)v_pal_mode != pal_mode) {
                pal_mode = (bool)v_pal_mode;
                cfg_dirty = true;
                snprintf(osd_text, sizeof(osd_text),
                          pal_mode ? "PAL  next launch" : "NTSC next launch");
                osd_text_ms = 1500;
            }
            /* The Overclock choice writes the per-cart override. 0 =
             * "use global"; non-zero = explicit MHz that this cart
             * always launches at. */
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
                    char path[NES_PICKER_PATH_MAX];
                    make_sidecar_path(path, sizeof(path), name, ".sta");
                    int rc = nesc_save_state(path);
                    nes_flash_disk_flush();
                    snprintf(osd_text, sizeof(osd_text),
                              rc == 0 ? "state saved" : "save fail");
                    osd_text_ms = 1000;
                    break;
                }
                case ACT_LOAD_STATE: {
                    char path[NES_PICKER_PATH_MAX];
                    make_sidecar_path(path, sizeof(path), name, ".sta");
                    int rc = nesc_load_state(path);
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

            /* Re-anchor frame pacing so the cart doesn't sprint to
             * catch up with the time we spent in the menu. */
            next_frame = get_absolute_time();
            last_input_us = (uint64_t)time_us_64();
        }

        /* Input gating: in CROP mode the cart receives nothing — the
         * D-pad pans the viewport instead so the user can read text
         * without the game stealing inputs. The cart still ticks. */
        if (scale_mode == SCALE_CROP) {
            nesc_set_buttons(0);
            const int STEP = 4;   /* px per frame while held */
            if (!menu_down) {
                if (up_down)    pan_y -= STEP;
                if (dn_down)    pan_y += STEP;
                if (!gpio_get(BTN_LEFT_GP))  pan_x -= STEP;
                if (!gpio_get(BTN_RIGHT_GP)) pan_x += STEP;
                if (pan_x < 0)   pan_x = 0;
                if (pan_x > 128) pan_x = 128;
                if (pan_y < 0)   pan_y = 0;
                if (pan_y > 112) pan_y = 112;
            }
        } else {
            nesc_set_buttons(read_nes_buttons());
        }

        /* In CROP mode the cart is paused — skip the emulator step
         * entirely so the user can read text without the game state
         * advancing. NES always boots in FIT (cfg never restores
         * scale_mode) so the buffer is populated by the time the
         * user can toggle to CROP. Fast-forward otherwise runs 4
         * frames per outer iteration. */
        if (scale_mode != SCALE_CROP) {
            int frame_runs = fast_forward ? 4 : 1;
            for (int i = 0; i < frame_runs; i++) nesc_run_frame();
            unsaved_play_frames += frame_runs;
        }

        /* Video out. */
        const uint8_t *frame = nesc_framebuffer();
        if (frame) {
            nes_lcd_wait_idle();
            if      (scale_mode == SCALE_CROP) blit_crop (fb, frame, pitch, pal, pan_x, pan_y);
            else if (blend)                    blit_blend(fb, frame, pitch, pal);
            else                                blit_fit  (fb, frame, pitch, pal);
            /* FPS overlay — top-left corner of the visible area.
             * Drawn directly into the RGB565 framebuffer so it
             * costs nothing extra. Toggle via MENU + LB chord. */
            if (show_fps) {
                char ftxt[12];
                snprintf(ftxt, sizeof(ftxt), "%d%s", fps_show,
                         fast_forward ? " FF" : "");
                nes_font_draw(fb, ftxt, 2, 5, 0xFFE0);   /* yellow */
            }
            if (osd_text_ms > 0) {
                int w = nes_font_width(osd_text);
                nes_font_draw(fb, osd_text, (128 - w) / 2, 60, 0xFFE0);
                osd_text_ms -= 16;
            }
            nes_lcd_present(fb);
        }

        /* Audio out. Skipped in CROP mode (paused) so the speaker
         * goes silent rather than holding the last frame's samples. */
        if (scale_mode != SCALE_CROP) {
            int n = nesc_audio_pull(audio, 1024);
            if (n > 0) {
                scale_audio(audio, n, volume);
                nes_audio_pwm_push(audio, n);
            }
        }

        /* Auto-save battery every AUTOSAVE_INTERVAL of wall time
         * when there have been play frames since the last save. */
        if (unsaved_play_frames > 0 &&
            (uint64_t)time_us_64() - last_autosave_us > AUTOSAVE_INTERVAL_US) {
            battery_save(name);
            last_autosave_us    = (uint64_t)time_us_64();
            unsaved_play_frames = 0;
        }

        /* Frame pacing: cap at 60 fps unless the user asked for
         * fast-forward. sleep_until is a no-op if we're already
         * past the target — i.e. when the emulator is the bottleneck. */
        fps_frames++;
        if (!fast_forward) {
            next_frame = delayed_by_us(next_frame, FRAME_US);
            sleep_until(next_frame);
        } else {
            /* Reset the pacing anchor so when the user toggles back
             * to normal speed we don't immediately try to "catch up"
             * the missed frames. */
            next_frame = get_absolute_time();
        }

        /* Update the on-screen FPS once per second. */
        if (absolute_time_diff_us(fps_window, get_absolute_time()) > 1000000) {
            fps_show   = fps_frames;
            fps_frames = 0;
            fps_window = get_absolute_time();
        }
    }

    /* Persist the battery save before tearing the cart down. */
    battery_save(name);
    if (cfg_dirty) cfg_save(name, scale_mode, show_fps, palette, volume, blend, pal_mode, cart_clock_mhz);
    /* Make sure the backlight is on for whatever comes next. */
    nes_lcd_backlight(1);

    nesc_shutdown();
    free(rom_alloc);

    /* Block until MENU is released so the lobby doesn't pick the
     * same press up as a navigation event. */
    while (!gpio_get(BTN_MENU_GP)) sleep_ms(10);
    return 0;
}
