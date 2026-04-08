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

#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

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

/* 256×240 → 128×120 nearest-neighbor downscale, centered with 4 px
 * black letterbox top + bottom. The source framebuffer is 8-bit
 * palette indices with NES_SCREEN_PITCH (= 272) stride and 8 px
 * of overdraw on the left edge. We sample every other row + col
 * and look up RGB565 from the palette in one pass. */
static void blit_scaled(uint16_t *fb, const uint8_t *src, int pitch,
                         const uint16_t *pal) {
    /* Top letterbox. */
    memset(fb, 0, 128 * 4 * 2);
    /* 120 visible rows starting at fb row 4. */
    for (int dy = 0; dy < 120; dy++) {
        const uint8_t *srow = src + (dy * 2) * pitch + 8 /* overdraw */;
        uint16_t      *drow = fb + (4 + dy) * 128;
        for (int dx = 0; dx < 128; dx++) {
            drow[dx] = pal[srow[dx * 2]];
        }
    }
    /* Bottom letterbox (4 px). */
    memset(fb + (4 + 120) * 128, 0, 128 * 4 * 2);
}

int nes_run_rom(const char *name, uint16_t *fb) {
    /* Slurp the ROM. Caller-side malloc; freed before return. */
    size_t   sz  = 0;
    uint8_t *rom = nes_picker_load_rom(name, &sz);
    if (!rom) return -1;

    if (nesc_init(22050) != 0)         { free(rom); return -2; }
    if (nesc_load_rom(rom, sz) != 0)   { free(rom); return -3; }

    const uint16_t *pal   = nesc_palette_rgb565();
    int             pitch = nesc_framebuffer_pitch();

    /* MENU exit detection: require ~500 ms hold so a single
     * accidental press doesn't bail. */
    int menu_held_ms = 0;

    /* Per-frame audio scratch. 22050 / 60 ≈ 368 samples per frame. */
    int16_t audio[1024];

    while (1) {
        /* Input. */
        nesc_set_buttons(read_nes_buttons());

        /* Exit on long MENU hold. */
        if (!gpio_get(BTN_MENU_GP)) {
            menu_held_ms += 16;
            if (menu_held_ms > 500) break;
        } else {
            menu_held_ms = 0;
        }

        /* Run one NTSC frame. */
        nesc_run_frame();

        /* Video out. */
        const uint8_t *frame = nesc_framebuffer();
        if (frame) {
            nes_lcd_wait_idle();
            blit_scaled(fb, frame, pitch, pal);
            nes_lcd_present(fb);
        }

        /* Audio out. */
        int n = nesc_audio_pull(audio, 1024);
        if (n > 0) nes_audio_pwm_push(audio, n);
    }

    nesc_shutdown();
    free(rom);
    return 0;
}
