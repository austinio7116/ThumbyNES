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

/* Progress callback for MD save/load state. PicoDrive's state.c calls
 * PicoStateProgressCB (a global fn-pointer it defines) before each
 * chunk write, passing a string like "Saving.. RAM" or "Loading.. VDP".
 * We repaint the LCD with the current string so the user can see where
 * the operation is sitting in real time — especially useful when the
 * 140 KB MD dump stalls mid-save on a flash-sector commit, or if a
 * specific chunk actually hangs indefinitely. md_save_progress_fb is
 * set by the save/load action handler to the live framebuffer. */
uint16_t *md_save_progress_fb;
void md_save_progress_cb(const char *str) {
    if (!md_save_progress_fb || !str) return;
    /* Two-line banner so short chunk names fit centred. Clear a wider
     * strip than the normal 1-line OSD — the strings are up to ~22
     * chars ("Saving.. FM_TIMERS" etc.). */
    memset(md_save_progress_fb + 56 * 128, 0, NES_FONT_CELL_H * 3 * 128 * 2);
    int tw = nes_font_width(str);
    if (tw > 126) tw = 126;
    nes_font_draw(md_save_progress_fb, str, (128 - tw) / 2, 60, 0xFFE0);
    nes_lcd_wait_idle();
    nes_lcd_present(md_save_progress_fb);
    nes_lcd_wait_idle();
}

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
static uint16_t read_md_buttons(void) {
    uint16_t m = 0;
    if (!gpio_get(BTN_UP_GP))    m |= MDC_BTN_UP;
    if (!gpio_get(BTN_DOWN_GP))  m |= MDC_BTN_DOWN;
    if (!gpio_get(BTN_LEFT_GP))  m |= MDC_BTN_LEFT;
    if (!gpio_get(BTN_RIGHT_GP)) m |= MDC_BTN_RIGHT;
    /* MD pad → Thumby mapping:
     *   Thumby B   → MD A     (primary action — Sonic jump)
     *   Thumby A   → MD B
     *   Thumby LB  → MD START  (was MODE; changed 2026-04-23 — START
     *                           on its own is more useful than MODE
     *                           since the 6-button sub-mode is rarely
     *                           needed, and replaces the earlier
     *                           LB+RB chord which was awkward.)
     *   Thumby RB  → MD C
     *   Thumby MENU (short tap) → cycle FIT / FILL / CROP display mode
     *   Thumby MENU (long hold) → in-game menu
     * If you genuinely need MODE (arcade games' "select/service"),
     * turn on the "6-button" toggle in the in-game menu — that frees
     * up the X/Y/Z/MODE bits again (still no Thumby binding for them
     * today but they stop being stripped so a future rebind has
     * somewhere to go). */
    if (!gpio_get(BTN_B_GP))     m |= MDC_BTN_A;
    if (!gpio_get(BTN_A_GP))     m |= MDC_BTN_B;
    if (!gpio_get(BTN_LB_GP))    m |= MDC_BTN_START;
    if (!gpio_get(BTN_RB_GP))    m |= MDC_BTN_C;
    return m;
}

/* Scaling lives in md_core's per-line PicoScanEnd callback — see
 * mdc_set_scale_target(). md_run just hands it the LCD framebuffer,
 * viewport, pan, and mode; PicoDrive streams each scanline through
 * our downsample and never materialises a full 320x240 frame. */


/* --- per-ROM cfg --------------------------------------------------- */

/* CFG_MAGIC bumped to 0x4D444558 ('MDEX') — replaced `frameskip`
 * field with `audio_mode` (FULL/HALF/OFF). Older MDEW saves still
 * load but fall back to audio_mode=FULL. */
#define CFG_MAGIC    0x4D444558u  /* 'MDEX' — audio_mode replaces frameskip */
#define CFG_MAGIC_V1 0x4D444557u  /* 'MDEW' — frameskip-era */
#define CFG_MAGIC_V0 0x4D444556u  /* 'MDEV' — pre-blend */

#define VOL_MIN    0
#define VOL_UNITY 15
#define VOL_MAX  30
#define VOL_DEF  15

typedef enum { SCALE_FIT = 0, SCALE_FILL = 1, SCALE_CROP = 2, SCALE_COUNT } scale_mode_t;
/* Audio mode: runtime-selectable quality vs speed tradeoff.
 *   FULL  = 22050 Hz YM2612/PSG synthesis — reference.
 *   HALF  = 11025 Hz — halves FM synthesis cost (~2.5 ms), audible
 *           HF roll-off but musical.
 *   OFF   = no Z80 dispatch + no FM + no PSG — fastest (locks 50
 *           PAL), completely silent. For twitchy action play. */
typedef enum { AUDIO_FULL = 0, AUDIO_HALF = 1, AUDIO_OFF = 2, AUDIO_COUNT } audio_mode_t;

typedef struct {
    uint32_t magic;
    uint8_t  scale_mode;
    uint8_t  show_fps;
    uint8_t  volume;
    uint8_t  six_button;     /* 0 = 3-button pad, 1 = 6-button */
    uint8_t  audio_mode;     /* 0=FULL, 1=HALF, 2=OFF */
    uint8_t  blend;          /* 0 = nearest, 1 = 2x2 packed-RGB565 blend */
    uint8_t  reserved[2];
    uint16_t clock_mhz;      /* 0 = use global; otherwise per-cart override */
    uint16_t _pad2;
} md_cfg_t;

static void cfg_load(const char *rom_name, scale_mode_t *scale,
                      bool *show_fps, int *volume, bool *six_btn,
                      int *audio_mode, bool *blend, int *clock_mhz) {
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
    if (c.magic == CFG_MAGIC || c.magic == CFG_MAGIC_V1 || c.magic == CFG_MAGIC_V0) {
        *show_fps = c.show_fps != 0;
        if (c.volume <= VOL_MAX) *volume = c.volume;
        *six_btn  = c.six_button != 0;
        if (clock_mhz) *clock_mhz = c.clock_mhz;
        /* blend available from V1 onwards. audio_mode from CFG_MAGIC
         * onwards; older saves inherit default (FULL). Old MDEW
         * `frameskip` byte sits in the same offset as audio_mode —
         * discard its value rather than mapping, since 0 (no skip)
         * happens to == FULL which is safe anyway. */
        if (c.magic != CFG_MAGIC_V0 && blend) *blend = c.blend != 0;
        if (c.magic == CFG_MAGIC && audio_mode && c.audio_mode < AUDIO_COUNT)
            *audio_mode = c.audio_mode;
    }
    f_close(&f);
}

static void cfg_save(const char *rom_name, scale_mode_t scale,
                      bool show_fps, int volume, bool six_btn,
                      int audio_mode, bool blend, int clock_mhz) {
    (void)scale;
    char path[NES_PICKER_PATH_MAX];
    make_sidecar_path(path, sizeof(path), rom_name, ".cfg");
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    md_cfg_t c = {
        .magic = CFG_MAGIC, .scale_mode = (uint8_t)SCALE_FIT,
        .show_fps = show_fps ? 1 : 0, .volume = (uint8_t)volume,
        .six_button = six_btn ? 1 : 0, .audio_mode = (uint8_t)audio_mode,
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

    /* Load the per-cart .cfg FIRST so we know the desired audio_mode
     * before initialising PicoDrive — audio_mode controls sample
     * rate (FULL/HALF) and opt flags (OFF disables Z80+FM+PSG). */
    int  volume       = VOL_DEF;
    bool show_fps     = false;
    bool fast_forward = false;
    bool six_button   = false;
    int  audio_mode   = AUDIO_FULL;
    bool blend        = true;   /* default on; toggle for crispness */
    scale_mode_t scale_mode = SCALE_FIT;
    int  cart_clock_mhz = 0;
    cfg_load(name, &scale_mode, &show_fps, &volume, &six_button,
             &audio_mode, &blend, &cart_clock_mhz);

    /* Audio mode → sample rate + opt flags. FULL/HALF keep the full
     * Z80+FM+PSG path at 22050/11025 Hz respectively. OFF passes
     * sample_rate=0 which mdc_init interprets as "no audio" and
     * masks out POPT_EN_Z80|POPT_EN_FM|POPT_EN_PSG. */
    int sample_rate = (audio_mode == AUDIO_HALF) ? 11025
                    : (audio_mode == AUDIO_OFF ) ? 0
                    :                               22050;
    if (mdc_init(MDC_REGION_AUTO, sample_rate) != 0) return -2;
    if (mdc_load_rom_xip(rom_const, sz) != 0)        return -3;
    battery_load(name);

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

    /* The scan callback only writes the active rect, so in FIT mode
     * the letterbox rows stay whatever they were. `fb_needs_clear`
     * forces a one-time full wipe on entry, after the menu closes
     * (menu drew over fb), and on scale_mode changes (old mode's
     * pixels outside the new active rect would otherwise linger).
     * Writing the letterbox every frame was wasted work AND opened a
     * tiny race with the still-in-flight DMA reading the top rows. */
    bool fb_needs_clear = true;
    bool prev_show_fps  = false;

    /* CROP pan (source coords). Centred default. */
    int pan_x = 96, pan_y = 48;
    int prev_a = 0;

    int16_t audio[1024];

    const uint32_t FRAME_US = 1000000u / (uint32_t)mdc_refresh_rate();
    absolute_time_t next_frame = get_absolute_time();
    absolute_time_t fps_window = get_absolute_time();
    int fps_frames = 0, fps_show = 0;
    int frame_tick = 0;

    /* Per-frame phase timers, summed across the 1-second window and
     * divided by fps_frames at rollover. Numbers shown on the second
     * FPS overlay line. time_us_32 reads the hardware us timer —
     * ~100 ns per call, negligible against a 25 ms frame. */
    uint32_t phase_emu_acc  = 0, phase_pres_acc = 0, phase_aud_acc = 0;
    uint32_t phase_emu_show = 0, phase_pres_show = 0, phase_aud_show = 0;

    /* Adaptive VDP-skip: if the previous frame overran the refresh
     * budget, we skip the VDP render pass on the next one to catch
     * up. 68K+Z80+audio continue normally — only the line composite
     * + FinalizeLine are elided, saving ~6-8 ms. LCD holds last fb
     * for one extra refresh, so at PAL 50 Hz a ~20 ms hold is
     * usually imperceptible in action gameplay. */
    uint32_t prev_emu_us    = 0;
    uint32_t skip_streak    = 0;   /* cap consecutive skips so we don't freeze */
    uint32_t skipped_count  = 0;   /* diagnostic: frames skipped in window */
    uint32_t skipped_show   = 0;
    const int  SKIP_BUDGET_US = (int)FRAME_US;      /* skip if we overran */
    const int  SKIP_STREAK_MAX = 2;                 /* never skip > 2 in a row */

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

        /* CROP pan is always-on — the d-pad pans the viewport as long
         * as the display is in CROP mode, no MENU required. The cart
         * keeps running (we can't pause it) but the cart never sees
         * the d-pad in CROP mode — it's stripped before feeding
         * PicoDrive, below. Face buttons / Start / C still reach the
         * cart so it continues to behave. Useful for exploring HUDs
         * or signage that lives outside the 128-col CROP window. */
        if (scale_mode == SCALE_CROP) {
            const int PAN_STEP = 2;
            int vx, vy, vw, vh;
            mdc_viewport(&vx, &vy, &vw, &vh);
            if (up_down) pan_y -= PAN_STEP;
            if (dn_down) pan_y += PAN_STEP;
            if (lt_down) pan_x -= PAN_STEP;
            if (rt_down) pan_x += PAN_STEP;
            int pmax_x = vw - 128; if (pmax_x < 0) pmax_x = 0;
            int pmax_y = vh - 128; if (pmax_y < 0) pmax_y = 0;
            if (pan_x < 0)       pan_x = 0;
            if (pan_x > pmax_x)  pan_x = pmax_x;
            if (pan_y < 0)       pan_y = 0;
            if (pan_y > pmax_y)  pan_y = pmax_y;
        }

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
                    fb_needs_clear = true;
                }
                menu_press_ms = 0;
                menu_was_down = 0;
                menu_consumed = 0;
            }
        }

        /* LB → MD START is now a direct gpio read inside
         * read_md_buttons — no chord, no pulse needed. Held LB gives
         * held START (e.g. menu select hold, arcade "press START to
         * continue" prompts). */
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
            int v_audio = audio_mode;
            int v_clock = 0;
            if (cart_clock_mhz == 125) v_clock = 1;
            if (cart_clock_mhz == 150) v_clock = 2;
            if (cart_clock_mhz == 200) v_clock = 3;
            if (cart_clock_mhz == 250) v_clock = 4;
            if (cart_clock_mhz == 300) v_clock = 5;

            static const char * const clock_choices[]   = { "global","125MHz","150MHz","200MHz","250MHz","300MHz" };
            static const int          clock_mhz_arr[]   = {  0,       125,     150,     200,     250,     300 };
            /* Audio mode choices. OFF disables Z80+FM+PSG entirely —
             * caps 50 PAL / 60 NTSC with zero audio path cost. HALF
             * runs YM2612 at 11025 Hz (upsampled ZOH in mdc_audio_pull)
             * for ~2.5 ms savings + mild HF roll-off. */
            static const char * const audio_choices[]   = { "FULL", "HALF", "OFF" };

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
                { .kind = NES_MENU_KIND_CHOICE, .label = "Audio",
                  .value_ptr = &v_audio, .choices = audio_choices, .num_choices = 3,
                  .enabled = true, .suffix = "next launch" },
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
            if (new_scale != scale_mode) { scale_mode = new_scale; cfg_dirty = true; fb_needs_clear = true; }
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
            if (v_audio != audio_mode)        { audio_mode   = v_audio;     cfg_dirty = true;
                                                /* audio_mode takes effect on next launch — the
                                                 * sample rate is baked into PicoDrive's PsndRerate
                                                 * tables, and OFF requires masking opt flags pre-
                                                 * PicoCartInsert. Persist to cfg and let the user
                                                 * relaunch the ROM. */ }
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
                    /* MD state dumps ~140 KB (RAM 64K + VRAM 64K + Z80 +
                     * VDP + PSG + FM tables). Per-chunk progress hook
                     * lands the current chunk name in osd_text and
                     * repaints the LCD so we can see exactly where a
                     * slow write sits — and if it actually hangs, which
                     * chunk is the culprit. Also flush the flash-disk
                     * cache up-front so the save starts with clean
                     * cache slots and the first 32 KB goes through
                     * without triggering commits. */
                    extern void (*PicoStateProgressCB)(const char *str);
                    extern uint16_t *md_save_progress_fb;
                    extern void md_save_progress_cb(const char *str);
                    md_save_progress_fb = fb;
                    PicoStateProgressCB = md_save_progress_cb;

                    nes_flash_disk_flush();
                    {
                        const char *txt = "saving state";
                        int tw = nes_font_width(txt);
                        memset(fb + 58 * 128, 0, NES_FONT_CELL_H * 128 * 2);
                        nes_font_draw(fb, txt, (128 - tw) / 2, 60, 0xFFE0);
                        nes_lcd_wait_idle();
                        nes_lcd_present(fb);
                        nes_lcd_wait_idle();
                    }

                    int rc = mdc_save_state(sta_path);
                    PicoStateProgressCB = NULL;
                    nes_flash_disk_flush();
                    snprintf(osd_text, sizeof(osd_text),
                              rc == 0 ? "state saved" : "save fail");
                    osd_text_ms = 1000;
                    break;
                }
                case ACT_LOAD_STATE: {
                    extern void (*PicoStateProgressCB)(const char *str);
                    extern uint16_t *md_save_progress_fb;
                    extern void md_save_progress_cb(const char *str);
                    md_save_progress_fb = fb;
                    PicoStateProgressCB = md_save_progress_cb;

                    {
                        const char *txt = "loading state";
                        int tw = nes_font_width(txt);
                        memset(fb + 58 * 128, 0, NES_FONT_CELL_H * 128 * 2);
                        nes_font_draw(fb, txt, (128 - tw) / 2, 60, 0xFFE0);
                        nes_lcd_wait_idle();
                        nes_lcd_present(fb);
                        nes_lcd_wait_idle();
                    }

                    int rc = mdc_load_state(sta_path);
                    PicoStateProgressCB = NULL;
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
            fb_needs_clear = true;       /* menu drew over fb */
        }

        /* Feed buttons. In CROP the d-pad pans the viewport instead of
         * steering the cart, so strip the direction bits before
         * PicoDrive sees them — face buttons / Start / C still reach
         * the cart. FIT / FILL pass the d-pad through unchanged. */
        uint16_t pad = read_md_buttons();
        if (scale_mode == SCALE_CROP) {
            pad &= ~(MDC_BTN_UP | MDC_BTN_DOWN | MDC_BTN_LEFT | MDC_BTN_RIGHT);
        }
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

        /* show_fps transitioned true→false in FIT: the last text sat
         * in the letterbox and the scan callback won't erase it, so
         * force a one-shot clear. */
        if (prev_show_fps && !show_fps && scale_mode == SCALE_FIT)
            fb_needs_clear = true;
        prev_show_fps = show_fps;

        if (fb_needs_clear) {
            memset(fb, 0, 128 * 128 * 2);
            fb_needs_clear = false;
        }
        mdc_set_scale_target(fb, (int)scale_mode, vx, vy, vw, vh,
                              pan_x, pan_y);

        /* Adaptive skip-render decision — set before running the
         * frame. Never skip under fast-forward (already running flat
         * out) or when audio=OFF (we're at 50 FPS locked anyway). */
        int skip_this = 0;
        if (!fast_forward
            && audio_mode != AUDIO_OFF
            && (int)prev_emu_us > SKIP_BUDGET_US
            && skip_streak < SKIP_STREAK_MAX)
        {
            skip_this = 1;
            skip_streak++;
            skipped_count++;
        } else {
            skip_streak = 0;
        }
        mdc_set_skip_render(skip_this);

        /* Run frames. Fast-forward emulates 4× the usual count; all
         * frames get rendered (the downsample is cheap enough). */
        uint32_t t_emu0 = time_us_32();
        int frame_runs = fast_forward ? 4 : 1;
        for (int i = 0; i < frame_runs; i++) mdc_run_frame();
        unsaved_play_frames += frame_runs;
        uint32_t t_emu1 = time_us_32();
        prev_emu_us = t_emu1 - t_emu0;

        /* Per-line callbacks have filled `fb`. Wait for any in-flight
         * DMA to drain, then overlay + present. */
        {
            nes_lcd_wait_idle();
            if (show_fps) {
                /* Overlay values are all rollover-sampled (once per
                 * 1-second window), so text width is stable and we
                 * can draw without a black-background wipe — each
                 * frame's redraw overwrites the same pixels. */
                /* Fixed-width format so the string length never changes —
                 * each frame's draw overwrites exactly the same pixels
                 * without leaving stale glyphs. One-char tag (space when
                 * FULL), 2-digit FPS, 5-digit emul us, 2-digit skipped. */
                char atag = fast_forward          ? 'F'
                          : (audio_mode == AUDIO_HALF) ? 'h'
                          : (audio_mode == AUDIO_OFF ) ? 'm'
                          :                              ' ';
                char ftxt[32];
                snprintf(ftxt, sizeof(ftxt), "%2d%c e%5u k%2u",
                         fps_show, atag,
                         (unsigned)phase_emu_show,
                         (unsigned)skipped_show);
                /* Strip-wipe behind text ONLY in FIT mode, where the
                 * wiped strip sits within the already-black letterbox
                 * (invisible). FILL / CROP draw over game content
                 * directly; fixed-width format keeps stale digits
                 * from appearing in practice. */
                if (scale_mode == SCALE_FIT)
                    memset(fb + 5 * 128, 0, NES_FONT_CELL_H * 128 * 2);
                nes_font_draw(fb, ftxt, 2, 5, 0xFFE0);
            }
            if (osd_text_ms > 0) {
                int w = nes_font_width(osd_text);
                nes_font_draw(fb, osd_text, (128 - w) / 2, 60, 0xFFE0);
                osd_text_ms -= 16;
            }
            nes_lcd_present(fb);
        }
        uint32_t t_pres = time_us_32();

        int n = mdc_audio_pull(audio, 1024);
        if (n > 0) {
            scale_audio(audio, n, volume);
            nes_audio_pwm_push(audio, n);
        }
        uint32_t t_aud = time_us_32();

        phase_emu_acc  += (t_emu1 - t_emu0);
        phase_pres_acc += (t_pres - t_emu1);
        phase_aud_acc  += (t_aud  - t_pres);

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
            if (fps_frames > 0) {
                phase_emu_show  = phase_emu_acc  / (uint32_t)fps_frames;
                phase_pres_show = phase_pres_acc / (uint32_t)fps_frames;
                phase_aud_show  = phase_aud_acc  / (uint32_t)fps_frames;
            }
            phase_emu_acc = phase_pres_acc = phase_aud_acc = 0;
            skipped_show = skipped_count; skipped_count = 0;
            fps_show = fps_frames; fps_frames = 0;
            fps_window = get_absolute_time();
        }
    }

    battery_save(name);
    if (cfg_dirty) cfg_save(name, scale_mode, show_fps, volume, six_button,
                             audio_mode, blend, cart_clock_mhz);
    nes_lcd_backlight(1);
    mdc_shutdown();
    while (!gpio_get(BTN_MENU_GP)) sleep_ms(10);
    return 0;
}
