/*
 * ThumbyNES — device-side PC Engine runner.
 *
 * SCAFFOLD ONLY. Wire-through target so the picker can dispatch to a
 * PCE stub without breaking the build. A functional implementation
 * must be built up in the following order:
 *
 *   1. Land PCE_SCANLINE_RENDER in pce_core.c (see PCE_PLAN.md §5 and
 *      task #12 "Investigate MY_VIDEO_MODE_SCANLINES path"). Until
 *      then a device build would allocate 220 KB for XBUF and OOM.
 *
 *   2. Port the gb_run.c skeleton — scale modes, battery load/save,
 *      save-state sidecars, MENU overlay, autosave-on-exit, idle-
 *      sleep, clock overrides. The shape is mechanical; it's just
 *      substituting pcec_* calls for gbc_*.
 *
 *   3. Add blit_fit_pce() and blit_crop_pce(). PCE viewport varies
 *      by game (256×224 is the 80% case; 336×240 Art of Fighting is
 *      the stretch case) — we'll default to FIT 2:1 with letterbox,
 *      like nes_run, and add a CROP mode for the oddballs.
 *
 * For now this file provides a single "not yet implemented" splash
 * so the rest of the picker machinery can compile and be tested.
 */
#include "pce_run.h"
#include "pce_core.h"
#include "nes_picker.h"
#include "nes_lcd_gc9107.h"
#include "nes_buttons.h"
#include "nes_font.h"

#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"

int pce_run_clock_override(const char *rom_name)
{
    (void)rom_name;
    /* No per-cart overclocks until we have a compat list. */
    return 0;
}

static void fill_bg(uint16_t *fb, uint16_t colour)
{
    for (int i = 0; i < 128 * 128; i++) fb[i] = colour;
}

int pce_run_rom(const nes_rom_entry *e, uint16_t *fb)
{
    (void)e;

    /* Navy background with a friendly "coming soon" message so users
     * dropping a .pce file see something coherent instead of a crash
     * or a frozen picker. */
    const uint16_t NAVY  = 0x0011;
    const uint16_t WHITE = 0xFFFF;

    fill_bg(fb, NAVY);
    nes_font_draw(fb,  8, 40, "PC ENGINE",   WHITE);
    nes_font_draw(fb,  8, 54, "NOT YET",     WHITE);
    nes_font_draw(fb,  8, 64, "IMPLEMENTED", WHITE);
    nes_font_draw(fb,  8, 86, "MENU = BACK", WHITE);
    nes_lcd_blit(fb);

    /* Wait for MENU to release, then return to the picker. */
    for (;;) {
        nes_buttons_read();
        if (nes_buttons_menu_just_released()) break;
        sleep_ms(16);
    }
    return 0;
}
