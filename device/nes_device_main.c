/*
 * ThumbyNES — Thumby Color device runtime entry point.
 *
 * Phase 3 scope:
 *   - Boots the chip at 250 MHz.
 *   - Brings up LCD + buttons + audio + flash disk + USB MSC.
 *   - Mounts the FAT volume (reformats only on mount failure).
 *   - Scans / for *.nes ROMs.
 *   - Shows the picker: either a "no ROMs" splash (with USB
 *     drag-and-drop active) or a scrollable list.
 *   - When the user picks a ROM, we display "selected: foo.nes"
 *     and hold there. Phase 4 will hand off to the Nofrendo core.
 *
 * Notes for the next phase: the Nofrendo runtime expects a 256×240
 * palette-indexed framebuffer plus a 256-entry RGB565 palette. The
 * blit hook in nes_core.c writes the latest frame; the device draw
 * loop will need to apply the 2:1 horizontal + 2:1 vertical scale
 * inline (see PLAN.md §4) into the 128×128 LCD framebuffer here.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#ifndef THUMBYONE_SLOT_MODE
#include "tusb.h"
#else
/* Slot mode: tinyUSB isn't linked. Stub tud_task() so the call
 * sites in the lobby loop + boot enumeration pump compile away. */
#define tud_task()  do { } while (0)
#endif
#include "ff.h"

#include "nes_lcd_gc9107.h"
#include "nes_buttons.h"
#include "nes_audio_pwm.h"
#include "nes_flash_disk.h"
#include "nes_picker.h"
#include "nes_font.h"
#include "nes_run.h"
#include "sms_run.h"
#include "gb_run.h"

#define THUMBYNES_VERSION "1.02"

/* Static framebuffer + filesystem state. */
static uint16_t       fb[128 * 128];
FATFS                 g_fs;   /* exported to nes_picker for XIP mmap */
static nes_rom_entry  roms[NES_PICKER_MAX_ROMS];


#ifndef THUMBYONE_SLOT_MODE
/* In ThumbyOne slot mode the NES slot doesn't own USB — the lobby
 * handles all file transfers. Stub out the MSC activity globals so
 * the rest of this file can reference them unconditionally, and
 * skip the tinyUSB init + pump calls below. */
extern volatile int      g_msc_ejected;
extern volatile uint64_t g_msc_last_op_us;
#else
static volatile int      g_msc_ejected    = 0;
static volatile uint64_t g_msc_last_op_us = 0;
#endif

/* --- helpers -------------------------------------------------------- */

static void fb_fill(uint16_t c) {
    for (int i = 0; i < 128 * 128; i++) fb[i] = c;
}

static void splash_color(uint16_t c) {
    fb_fill(c);
    nes_lcd_present(fb);
    nes_lcd_wait_idle();
}

static void splash_text(const char *line1, const char *line2, uint16_t bg) {
    fb_fill(bg);
    if (line1) nes_font_draw(fb, line1, (128 - nes_font_width(line1)) / 2, 56, 0xFFFF);
    if (line2) nes_font_draw(fb, line2, (128 - nes_font_width(line2)) / 2, 68, 0xFFFF);
    nes_lcd_present(fb);
    nes_lcd_wait_idle();
}

/* Boot splash: pure-text logo, drawn once as the first visible
 * frame. No internal sleep — the caller enforces a minimum on-screen
 * duration by deadline so the real init work underneath (USB
 * enumeration pump etc.) runs with the logo already visible. */
static void boot_splash(void) {
    fb_fill(0x0000);
    nes_font_draw(fb, "ThumbyNES",       32, 32, 0xFD20);  /* orange  */
    nes_font_draw(fb, "v" THUMBYNES_VERSION, 48, 44, 0xFFFF);
    nes_font_draw(fb, "NES emulator",    24, 64, 0xC618);  /* l.grey  */
    nes_font_draw(fb, "for the",          40, 72, 0x8410);  /* m.grey  */
    nes_font_draw(fb, "Thumby Color",    24, 80, 0xC618);
    nes_font_draw(fb, "nofrendo",        36, 100, 0x4208); /* d.grey  */
    nes_font_draw(fb, "GPLv2",           44, 108, 0x4208);
    nes_lcd_present(fb);
    nes_lcd_wait_idle();
}

/* --- filesystem boot ------------------------------------------------ */

/* Mount the flash disk. Reformat only if mount fails outright — any
 * other heuristic is unsafe because Windows mutates the BPB on mount.
 * Hold MENU at boot to force a wipe. */
static int boot_filesystem(void) {
    int force = nes_buttons_menu_pressed();

    int needs_format = force;
    FRESULT r = f_mount(&g_fs, "", 1);
    if (r != FR_OK) needs_format = 1;

#ifdef THUMBYONE_SLOT_MODE
    /* Under ThumbyOne, the lobby is the ONLY thing allowed to format
     * the shared FAT. Auto-format from a slot would wipe the user's
     * content across every system (roms/carts/games) whenever a
     * transient mount failure happened — a data-loss bug, not a
     * "self-recovery". Fail loudly instead: show a red splash and
     * ask the user to return to the lobby (where LB+RB at boot is
     * the documented wipe path).
     *
     * The MENU-held-at-boot force path is also blocked here — lobby
     * owns that chord too. */
    if (needs_format) {
        splash_text("FS error", "hold MENU at lobby", 0xf800);
        return -1;
    }
#else
    if (needs_format) {
        splash_color(0xffe0);   /* yellow = formatting */

        f_unmount("");
        memset(&g_fs, 0, sizeof(g_fs));

        BYTE work[FF_MAX_SS * 4];
        /* FAT16, 1 KB clusters → 12288 clusters, well above the
         * FAT12 4084-cluster cap. Forcing FAT16 sidesteps Windows
         * quirks with FAT12 on removable drives larger than 8 MB. */
        MKFS_PARM opt = { FM_FAT, 1, 0, 0, 1024 };
        if (f_mkfs("", &opt, work, sizeof(work)) != FR_OK) {
            splash_color(0xf800);   /* red = mkfs failed */
            return -1;
        }
        nes_flash_disk_flush();

        if (f_mount(&g_fs, "", 1) != FR_OK) {
            splash_color(0xfa00);   /* dark red = remount failed */
            return -1;
        }
        f_setlabel("THUMBYNES");
        nes_flash_disk_flush();
    }
#endif
    return 0;
}

/* --- main lobby loop ------------------------------------------------ */

/* Continuous background commit logic — pulled directly from the
 * proven ThumbyP8 lobby. Whenever MSC has been quiet for >300 ms,
 * commit one dirty cache block per iteration. The 50 ms IRQs-off
 * window of an erase is safe in those gaps because no transfer is
 * in flight. */
static void drain_one_if_quiet(void) {
    uint64_t now = (uint64_t)time_us_64();
    uint64_t since_op = now - g_msc_last_op_us;
    if (since_op > 300000 && nes_flash_disk_dirty()) {
        nes_flash_disk_commit_one();
    }
}

#ifdef THUMBYONE_SLOT_MODE
#include "thumbyone_handoff.h"

/* ThumbyOne slot mode: USB lives in the top-level lobby, so users
 * can only add ROMs by rebooting back to it. That makes the
 * standalone "wait here until you drop a .nes" polling loop dead
 * UX — we either have ROMs or we don't, no live arrival. Skip
 * straight to the picker when we do, and show a one-shot "no roms,
 * return to lobby" splash when we don't. MENU long-hold in that
 * splash returns to the lobby, same as everywhere else. */
static int lobby(void) {
    int n_roms = nes_picker_scan(roms, NES_PICKER_MAX_ROMS);

    if (n_roms == 0) {
        fb_fill(0x0000);
        nes_font_draw(fb, "ThumbyNES",     32, 20, 0xFD20);
        nes_font_draw(fb, "no roms in",    22, 48, 0xFFFF);
        nes_font_draw(fb, "/roms/",        50, 58, 0xFFFF);
        nes_font_draw(fb, "hold MENU to",  18, 88, 0x8410);
        nes_font_draw(fb, "return to lobby",12, 98, 0x8410);
        nes_font_draw(fb, "USB transfer",  18, 114, 0xFFE0);
        nes_font_draw(fb, "happens there", 18, 122, 0xFFE0);
        nes_lcd_present(fb);
        nes_lcd_wait_idle();

        /* Block until the user long-holds MENU (800 ms) to go back
         * to the top-level lobby. Short taps are ignored. */
        while (1) {
            if (nes_buttons_menu_pressed()) {
                absolute_time_t d = make_timeout_time_ms(800);
                bool held = true;
                while (nes_buttons_menu_pressed()) {
                    if (time_reached(d)) break;
                    sleep_ms(10);
                }
                held = time_reached(d);
                if (held) thumbyone_handoff_request_lobby();
                /* Wait for release so a still-held button doesn't
                 * instantly re-trigger after we come back. */
                while (nes_buttons_menu_pressed()) sleep_ms(10);
            }
            sleep_ms(30);
        }
    }

    int sel = nes_picker_run(fb, roms, &n_roms);
    return sel;   /* may be <0 if user deleted everything; main() retries */
}
#else
/* Standalone: pump USB + drain + scan loop. */
static int lobby(void) {
    absolute_time_t next_scan = make_timeout_time_ms(0);
    int n_roms = 0;
    int splash_drawn = 0;

    while (1) {
        tud_task();
        drain_one_if_quiet();

        if (absolute_time_diff_us(get_absolute_time(), next_scan) <= 0) {
            n_roms = nes_picker_scan(roms, NES_PICKER_MAX_ROMS);
            next_scan = make_timeout_time_ms(1000);
            splash_drawn = 0;   /* force redraw if count changed */
        }

        if (n_roms == 0) {
            if (!splash_drawn) {
                fb_fill(0x0000);
                nes_font_draw(fb, "ThumbyNES", 32, 24, 0xFD20);
                nes_font_draw(fb, "no roms",      36, 48, 0xFFFF);
                nes_font_draw(fb, "drop .nes via", 18, 70, 0x8410);
                nes_font_draw(fb, "usb drive",    30, 78, 0x8410);
                nes_font_draw(fb, "then eject",   28, 92, 0x8410);
                nes_lcd_present(fb);
                nes_lcd_wait_idle();
                splash_drawn = 1;
            }
            sleep_ms(16);
            continue;
        }

        /* At least one ROM present — hand off to the picker. The
         * picker has its own input + draw loop and pumps tud_task
         * itself, so we won't lose USB while it's up. The picker
         * mutates `roms` and `n_roms` in place when it re-scans on
         * USB activity or after a B-hold delete. */
        int sel = nes_picker_run(fb, roms, &n_roms);
        if (sel >= 0) return sel;
        /* sel < 0 → all ROMs deleted; bounce back to the no-roms splash. */
        next_scan = make_timeout_time_ms(0);
    }
}
#endif

/* --- entry point ---------------------------------------------------- */

int main(void) {
    /* RP2350 @ 250 MHz BEFORE stdio_init_all so the USB PLL
     * comes up at the right divider. */
    set_sys_clock_khz(250000, true);

    /* Initialises USB clocks even if we never write to stdout. */
    stdio_init_all();

    nes_buttons_init();
    nes_lcd_init();
    nes_audio_pwm_init();

    /* FAT must come up BEFORE USB. If we let the host start
     * enumerating while mkfs is mid-flight, Windows reads
     * inconsistent BPB state and File Explorer trips over the
     * half-written directory tree. The yellow "formatting" splash
     * inside boot_filesystem() only fires on first-boot / mount
     * failure; a normal boot mounts silently. */
    nes_flash_disk_init();
    if (boot_filesystem() < 0) {
        splash_color(0xf81f);   /* magenta = mount/format failed */
        while (1) tight_loop_contents();
    }
    nes_flash_disk_flush();

#ifndef THUMBYONE_SLOT_MODE
    /* Standalone boot: show the ThumbyNES logo for at least 1.2 s
     * and pump USB enumeration behind it. In slot mode we skip
     * this entirely — the ThumbyOne lobby already showed a logo
     * when the user selected NES, so a second "ThumbyNES" splash
     * is a second lobby screen for no reason. */
    boot_splash();
    absolute_time_t splash_until = make_timeout_time_ms(1200);

    /* USB stack. Disk is fully on flash → enumeration is safe. */
    tusb_init();
    {
        absolute_time_t until = make_timeout_time_ms(1000);
        while (!time_reached(until)) {
            tud_task();
            sleep_us(100);
        }
    }

    /* Hold the logo until the minimum has elapsed. Fast boot path
     * finishes USB before 1200 ms; slow path already blew past it. */
    while (!time_reached(splash_until)) sleep_ms(10);
#endif

    /* Old firmware versions wrote /.last for a quick-resume feature
     * that has since been removed. Sweep it away on boot so the
     * file doesn't sit around as cruft. Harmless if absent. */
    f_unlink("/.last");

    /* Apply the saved overclock from /.global. We bootstrap at the
     * 250 MHz default above so the LCD splashes can render before
     * the FAT volume is mounted; once we know the user's preference
     * we re-clock and re-init the timing-sensitive peripherals (LCD
     * SPI dividers and audio PWM IRQ rate). */
    {
        int target_mhz = nes_picker_global_clock_mhz();
        if (target_mhz != 250 && target_mhz >= 50 && target_mhz <= 400) {
            set_sys_clock_khz((uint32_t)target_mhz * 1000u, true);
            nes_lcd_init();
            nes_audio_pwm_init();
        }
    }

    /* The old "fragmented-ROM pre-flight + auto-defrag" pass used to
     * live here — it walked /roms/, tried nes_picker_mmap_rom() on
     * every file >64 KB, and rewrote any whose cluster chain came
     * back non-contiguous. In practice the detection was unreliable
     * (reported fragmentation when files were fine), so it fired on
     * every boot and flashed a "checking files" / "DEFRAGMENTING"
     * pair before the picker. The actual XIP-mmap failure it was
     * meant to avoid turned out to have a different root cause, so
     * there's no reason to keep the pre-flight. Removed entirely —
     * nes_picker_defrag() is still linked in case we want to wire it
     * to an explicit menu entry later. */

    /* Lobby + picker. Loops forever in Phase 3. */
    int current_clock_mhz = nes_picker_global_clock_mhz();
    while (1) {
        int sel = lobby();
        if (sel < 0) continue;

        /* Re-apply the saved overclock before launching. Preference
         * order:
         *   1. Per-cart override stored in the ROM's .cfg
         *   2. Global value in /.global
         * Clock changes have to happen between sessions because the
         * cart cores set up audio/LCD against the current sys_clock. */
        {
            int per_cart = 0;
            switch (roms[sel].system) {
            case ROM_SYS_NES: per_cart = nes_run_clock_override(roms[sel].name); break;
            case ROM_SYS_GB:  per_cart = gb_run_clock_override (roms[sel].name); break;
            default:          per_cart = sms_run_clock_override(roms[sel].name); break;
            }
            int target = per_cart ? per_cart : nes_picker_global_clock_mhz();
            if (target != current_clock_mhz
                && target >= 50 && target <= 400) {
                set_sys_clock_khz((uint32_t)target * 1000u, true);
                nes_lcd_init();
                nes_audio_pwm_init();
                current_clock_mhz = target;
            }
        }

        /* Hand off to the Nofrendo runner. Returns when the user
         * holds MENU; we then fall back through the lobby for
         * another pick. */
        int rc;
        switch (roms[sel].system) {
        case ROM_SYS_NES: rc = nes_run_rom(&roms[sel], fb); break;
        case ROM_SYS_GB:  rc = gb_run_rom (&roms[sel], fb); break;
        default:          rc = sms_run_rom(&roms[sel], fb); break;
        }
        if (rc != 0) {
            /* Visible error: red splash held until the user presses
             * any button, so they can actually read it instead of
             * blinking past a 1.5 s flash. */
            char line[40];
            snprintf(line, sizeof(line), "load err %d", rc);
            splash_text(line, roms[sel].name, 0xf800);
            while (nes_buttons_read() == 0 && !nes_buttons_menu_pressed())
                sleep_ms(20);
            while (nes_buttons_menu_pressed()) sleep_ms(10);
        }
        /* Wait for MENU release so the long-hold exit doesn't
         * immediately retrigger as a select in the picker. */
        while (nes_buttons_menu_pressed()) sleep_ms(10);
    }
}
