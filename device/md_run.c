/*
 * ThumbyNES — device-side Mega Drive / Genesis runner.
 *
 * Mirrors sms_run.c but drives PicoDrive instead of smsplus. Uses the
 * picker's XIP mmap path — ROM data stays in flash, PicoDrive reads
 * raw-BE bytes directly and byteswaps on access (see FAME_BIG_ENDIAN
 * in the vendored core). Scaling (FIT / FILL / CROP) is done inside
 * md_core's PicoScanEnd callback — see mdc_set_scale_target().
 */
#include "md_run.h"
#include "md_core.h"
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

/* Pin map — same as every other runner. */
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
    uint8_t *ram = mdc_battery_ram();
    size_t   sz  = mdc_battery_size();
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
    uint8_t *ram = mdc_battery_ram();
    size_t   sz  = mdc_battery_size();
    if (!ram || sz == 0) return;
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".sav");
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT bw = 0;
    f_write(&f, ram, (UINT)sz, &bw);
    f_close(&f);
}

/* Thumby physical buttons → MD pad mask. Default layout (3-button):
 *   Thumby A  → MD A   (jump / light attack in most titles)
 *   Thumby B  → MD B
 *   Thumby LB → MD C   (heavy attack)
 *   Thumby RB → MD X   (6-button only; defaults to unused)
 *   MENU      → START
 * The 68K code convention pairs jump on the rightmost face button — on
 * most MD titles that's B. Match that. */
/* One-frame Start pulse triggered by a short tap of MENU. Counts down
 * to zero in the main loop; while > 0 read_md_buttons OR's in START. */
static int s_start_pulse_frames = 0;

static uint16_t read_md_buttons(void) {
    uint16_t m = 0;
    if (!gpio_get(BTN_UP_GP))    m |= MDC_BTN_UP;
    if (!gpio_get(BTN_DOWN_GP))  m |= MDC_BTN_DOWN;
    if (!gpio_get(BTN_LEFT_GP))  m |= MDC_BTN_LEFT;
    if (!gpio_get(BTN_RIGHT_GP)) m |= MDC_BTN_RIGHT;
    /* MD pad → Thumby mapping:
     *   Thumby B   → MD A     (primary action — Sonic jump)
     *   Thumby A   → MD B
     *   Thumby RB  → MD C
     *   Thumby LB  → MD MODE  (acts as "select" on MD)
     *   Thumby MENU (short tap) → MD START (see s_start_pulse_frames)
     *   Thumby MENU (long hold) → in-game menu (unchanged) */
    if (!gpio_get(BTN_B_GP))     m |= MDC_BTN_A;
    if (!gpio_get(BTN_A_GP))     m |= MDC_BTN_B;
    if (!gpio_get(BTN_RB_GP))    m |= MDC_BTN_C;
    if (!gpio_get(BTN_LB_GP))    m |= MDC_BTN_MODE;
    if (s_start_pulse_frames > 0) m |= MDC_BTN_START;
    return m;
}

/* Scaling lives in md_core's per-line PicoScanEnd callback — see
 * mdc_set_scale_target(). md_run just hands it the LCD framebuffer,
 * viewport, pan, and mode; PicoDrive streams each scanline through
 * our downsample and never materialises a full 320x240 frame. */


/* --- per-ROM cfg --------------------------------------------------- */

/* CFG_MAGIC bumped to 0x4D444557 ('MDEW') when adding `blend`.
 * Older saves still load (we accept them under the read guard) but
 * won't have the blend bit set — so default is on. */
#define CFG_MAGIC   0x4D444557u   /* 'MDEW' — was 'MDEV', added blend */
#define CFG_MAGIC_V0 0x4D444556u  /* old sidecar accepted on load */

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
    uint8_t  six_button;    /* 0 = 3-button pad, 1 = 6-button */
    uint8_t  frameskip;     /* 0, 1, or 2 */
    uint8_t  blend;         /* 0 = nearest, 1 = 2x2 packed-RGB565 blend */
    uint8_t  reserved[2];
    uint16_t clock_mhz;     /* 0 = use global; otherwise per-cart override */
    uint16_t _pad2;
} md_cfg_t;

static void cfg_load(const char *rom_name, scale_mode_t *scale,
                      bool *show_fps, int *volume, bool *six_btn,
                      int *frameskip, bool *blend, int *clock_mhz) {
    (void)scale;   /* scale_mode not restored across launches, same as SMS. */
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return;
    md_cfg_t c = {0};
    UINT br = 0;
    if (f_read(&f, &c, sizeof(c), &br) != FR_OK || br < 4) {
        f_close(&f);
        return;
    }
    if (c.magic == CFG_MAGIC || c.magic == CFG_MAGIC_V0) {
        *show_fps = c.show_fps != 0;
        if (c.volume <= VOL_MAX) *volume = c.volume;
        *six_btn   = c.six_button != 0;
        if (c.frameskip <= 2) *frameskip = c.frameskip;
        if (clock_mhz) *clock_mhz = c.clock_mhz;
        /* blend field only valid on new-magic saves. V0 keeps default. */
        if (c.magic == CFG_MAGIC && blend) *blend = c.blend != 0;
    }
    f_close(&f);
}

static void cfg_save(const char *rom_name, scale_mode_t scale,
                      bool show_fps, int volume, bool six_btn,
                      int frameskip, bool blend, int clock_mhz) {
    (void)scale;
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    md_cfg_t c = {
        .magic = CFG_MAGIC, .scale_mode = (uint8_t)SCALE_FIT,
        .show_fps = show_fps ? 1 : 0, .volume = (uint8_t)volume,
        .six_button = six_btn ? 1 : 0, .frameskip = (uint8_t)frameskip,
        .blend = blend ? 1 : 0, .reserved = {0,0},
        .clock_mhz = (uint16_t)clock_mhz, ._pad2 = 0,
    };
    UINT bw = 0;
    f_write(&f, &c, sizeof(c), &bw);
    f_close(&f);
}

int md_run_clock_override(const char *rom_name) {
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;
    md_cfg_t c = {0};
    UINT br = 0;
    f_read(&f, &c, sizeof(c), &br);
    f_close(&f);
    if (br < sizeof(c) || c.magic != CFG_MAGIC) return 0;
    return (int)c.clock_mhz;
}

/* MD audio is louder than SMS — no extra gain, just a linear
 * volume scale with clipping. Reuses the same VOL_* range as SMS. */
static void scale_audio(int16_t *buf, int n, int volume) {
    if (volume <= 0) { for (int i = 0; i < n; i++) buf[i] = 0; return; }
    for (int i = 0; i < n; i++) {
        int32_t s = (int32_t)buf[i] * volume / VOL_UNITY;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
}

int md_run_rom(const nes_rom_entry *e, uint16_t *fb) {
    const char *name = e->name;

    /* XIP mmap is the only viable path on device — a 2 MB ROM won't
     * fit in heap. If the file is fragmented, defrag inline before
     * giving up. */
    const uint8_t *rom_const = NULL;
    size_t         sz        = 0;
    int mmap_rc = nes_picker_mmap_rom(name, &rom_const, &sz);
    if (mmap_rc == -5) {
        /* -5 = fragmented. Try to compact and retry once. */
        nes_picker_defrag_one(name, fb);
        mmap_rc = nes_picker_mmap_rom(name, &rom_const, &sz);
    }
    if (mmap_rc != 0) return -30 + mmap_rc;

    if (mdc_init(MDC_REGION_AUTO, 22050) != 0) return -2;
    if (mdc_load_rom_xip(rom_const, sz) != 0)  return -3;
    battery_load(name);

    int  volume       = VOL_DEF;
    bool show_fps     = false;
    bool fast_forward = false;
    bool six_button   = false;
    int  frameskip    = 0;
    bool blend        = true;   /* default on; toggle for crispness */
    scale_mode_t scale_mode = SCALE_FIT;
    int  cart_clock_mhz = 0;
    cfg_load(name, &scale_mode, &show_fps, &volume, &six_button,
             &frameskip, &blend, &cart_clock_mhz);
    mdc_set_blend(blend ? 1 : 0);
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

    int  menu_press_ms = 0;
    int  menu_was_down = 0;
    int  menu_consumed = 0;
    int  open_menu     = 0;
    bool exit_after    = false;

    /* CROP pan (source coords). Centred default. */
    int pan_x = 96, pan_y = 48;
    int prev_a = 0;

    int16_t audio[1024];

    const uint32_t FRAME_US = 1000000u / (uint32_t)mdc_refresh_rate();
    absolute_time_t next_frame = get_absolute_time();
    absolute_time_t fps_window = get_absolute_time();
    int fps_frames = 0, fps_show = 0;
    int frame_tick = 0;

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
            /* CROP pan while MENU held. */
            if (scale_mode == SCALE_CROP) {
                const int PAN_STEP = 2;
                int vx, vy, vw, vh;
                mdc_viewport(&vx, &vy, &vw, &vh);
                if (up_down) { pan_y -= PAN_STEP; menu_consumed = 1; }
                if (dn_down) { pan_y += PAN_STEP; menu_consumed = 1; }
                if (lt_down) { pan_x -= PAN_STEP; menu_consumed = 1; }
                if (rt_down) { pan_x += PAN_STEP; menu_consumed = 1; }
                int pmax_x = vw - 128; if (pmax_x < 0) pmax_x = 0;
                int pmax_y = vh - 128; if (pmax_y < 0) pmax_y = 0;
                if (pan_x < 0)       pan_x = 0;
                if (pan_x > pmax_x)  pan_x = pmax_x;
                if (pan_y < 0)       pan_y = 0;
                if (pan_y > pmax_y)  pan_y = pmax_y;
            }
            if (menu_press_ms >= 500 && !menu_consumed) {
                open_menu = 1;
                menu_consumed = 1;
            }
        } else {
            if (menu_was_down) {
                if (!menu_consumed && menu_press_ms > 0 && menu_press_ms < 300) {
                    /* Short tap of MENU alone → cycle FIT/FILL/CROP. */
                    scale_mode = (scale_mode_t)((scale_mode + 1) % SCALE_COUNT);
                    cfg_dirty = true;
                }
                menu_press_ms = 0;
                menu_was_down = 0;
                menu_consumed = 0;
            }
        }

        /* LB+RB simultaneous chord → MD START pulse. Triggers on the
         * edge when both go from "not both held" to "both held". */
        {
            static int prev_lb_rb = 0;
            int both_now = lb_down && rb_down;
            if (both_now && !prev_lb_rb) s_start_pulse_frames = 2;
            prev_lb_rb = both_now;
        }
        if (s_start_pulse_frames > 0) s_start_pulse_frames--;
        prev_a = a_down;

        /* ----- in-game menu ----- */
        if (open_menu) {
            open_menu = 0;
            static const char * const display_choices[] = { "FIT", "FILL", "CROP" };
            int v_scale = scale_mode;
            int v_vol   = volume;
#ifdef THUMBYONE_SLOT_MODE
            int v_bri   = thumbyone_settings_load_brightness();
            int old_bri = v_bri;
#endif
            int v_ff    = fast_forward ? 1 : 0;
            int v_fps   = show_fps ? 1 : 0;
            int v_6btn  = six_button ? 1 : 0;
            int v_blend = blend ? 1 : 0;
            int v_skip  = frameskip;
            int v_clock = 0;
            if (cart_clock_mhz == 125) v_clock = 1;
            if (cart_clock_mhz == 150) v_clock = 2;
            if (cart_clock_mhz == 200) v_clock = 3;
            if (cart_clock_mhz == 250) v_clock = 4;
            if (cart_clock_mhz == 300) v_clock = 5;

            static const char * const clock_choices[]   = { "global","125MHz","150MHz","200MHz","250MHz","300MHz" };
            static const int          clock_mhz_arr[]   = {  0,       125,     150,     200,     250,     300 };
            static const char * const skip_choices[]    = { "0", "1", "2" };

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
                  .value_ptr = &v_scale, .choices = display_choices, .num_choices = 3,
                  .enabled = true },
                { .kind = NES_MENU_KIND_SLIDER, .label = "Volume",
                  .value_ptr = &v_vol, .min = VOL_MIN, .max = VOL_MAX,
                  .enabled = true },
#ifdef THUMBYONE_SLOT_MODE
                { .kind = NES_MENU_KIND_SLIDER, .label = "Brightness",
                  .value_ptr = &v_bri, .min = 0, .max = 255, .enabled = true },
#endif
                { .kind = NES_MENU_KIND_TOGGLE, .label = "Fast-fwd",
                  .value_ptr = &v_ff, .enabled = true },
                { .kind = NES_MENU_KIND_TOGGLE, .label = "Show FPS",
                  .value_ptr = &v_fps, .enabled = true },
                { .kind = NES_MENU_KIND_TOGGLE, .label = "6-button",
                  .value_ptr = &v_6btn, .enabled = true },
                { .kind = NES_MENU_KIND_TOGGLE, .label = "BLEND",
                  .value_ptr = &v_blend, .enabled = true },
                { .kind = NES_MENU_KIND_CHOICE, .label = "Frameskip",
                  .value_ptr = &v_skip, .choices = skip_choices, .num_choices = 3,
                  .enabled = true },
                { .kind = NES_MENU_KIND_CHOICE, .label = "Overclock",
                  .value_ptr = &v_clock, .choices = clock_choices, .num_choices = 6,
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

            scale_mode_t new_scale = (scale_mode_t)v_scale;
            if (new_scale != scale_mode) { scale_mode = new_scale; cfg_dirty = true; }
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
            if ((bool)v_ff   != fast_forward) { fast_forward = (bool)v_ff;       }
            if ((bool)v_fps  != show_fps )    { show_fps     = (bool)v_fps;  cfg_dirty = true; }
            if ((bool)v_6btn != six_button)   { six_button   = (bool)v_6btn;cfg_dirty = true; }
            if ((bool)v_blend!= blend)        { blend        = (bool)v_blend;cfg_dirty = true;
                                                mdc_set_blend(blend ? 1 : 0); }
            if (v_skip != frameskip)          { frameskip    = v_skip;      cfg_dirty = true; }
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
                    int rc = mdc_save_state(sta_path);
                    nes_flash_disk_flush();
                    snprintf(osd_text, sizeof(osd_text),
                              rc == 0 ? "state saved" : "save fail");
                    osd_text_ms = 1000;
                    break;
                }
                case ACT_LOAD_STATE: {
                    int rc = mdc_load_state(sta_path);
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

        /* Feed buttons; in CROP the cart gets no input while MENU held
         * (MENU+dpad pans). Otherwise cart receives the input. */
        uint16_t pad = 0;
        if (!(scale_mode == SCALE_CROP && menu_down))
            pad = read_md_buttons();
        if (!six_button) {
            /* Strip the 6-button-only bits so the cart reads 3-button. */
            pad &= ~(MDC_BTN_X | MDC_BTN_Y | MDC_BTN_Z | MDC_BTN_MODE);
        }
        mdc_set_buttons(pad);

        /* Set up the line-scratch downsample target for this frame.
         * PicoDrive will write each MD scanline into md_core's 640-byte
         * scratch and our PicoScanEnd callback streams the downsample
         * directly into `fb` (128x128 LCD) — no intermediate frame. */
        int vx, vy, vw, vh;
        mdc_viewport(&vx, &vy, &vw, &vh);
        /* Clear letterbox columns/rows once per frame (FIT/CROP). The
         * scanline callback only writes the active dest rect. */
        if (scale_mode == SCALE_FIT)  memset(fb, 0, 128 * 19 * 2),
                                       memset(fb + (19+90)*128, 0, 128*19*2);
        else if (scale_mode == SCALE_CROP) memset(fb, 0, 128 * 128 * 2);
        mdc_set_scale_target(fb, (int)scale_mode, vx, vy, vw, vh,
                              pan_x, pan_y);

        /* Run frames. Frameskip = N means "render 1 of every (N+1)",
         * but the 68K still emulates every frame for timing. */
        int frame_runs = fast_forward ? 4 : (1 + frameskip);
        for (int i = 0; i < frame_runs; i++) mdc_run_frame();
        unsaved_play_frames += frame_runs;

        /* Per-line callbacks have filled `fb`. Wait for any in-flight
         * DMA to drain, then overlay + present. */
        {
            nes_lcd_wait_idle();
            if (show_fps) {
                char ftxt[16];
                snprintf(ftxt, sizeof(ftxt), "%d%s", fps_show,
                         fast_forward ? " FF" : (frameskip ? " FS" : ""));
                nes_font_draw(fb, ftxt, 2, 5, 0xFFE0);
            }
            if (osd_text_ms > 0) {
                int w = nes_font_width(osd_text);
                nes_font_draw(fb, osd_text, (128 - w) / 2, 60, 0xFFE0);
                osd_text_ms -= 16;
            }
            nes_lcd_present(fb);
        }

        int n = mdc_audio_pull(audio, 1024);
        if (n > 0) {
            scale_audio(audio, n, volume);
            nes_audio_pwm_push(audio, n);
        }

        if (unsaved_play_frames > 0 &&
            (uint64_t)time_us_64() - last_autosave_us > AUTOSAVE_INTERVAL_US) {
            battery_save(name);
            last_autosave_us    = (uint64_t)time_us_64();
            unsaved_play_frames = 0;
        }

        frame_tick++;
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
    if (cfg_dirty) cfg_save(name, scale_mode, show_fps, volume, six_button,
                             frameskip, blend, cart_clock_mhz);
    nes_lcd_backlight(1);
    mdc_shutdown();
    while (!gpio_get(BTN_MENU_GP)) sleep_ms(10);
    return 0;
}
