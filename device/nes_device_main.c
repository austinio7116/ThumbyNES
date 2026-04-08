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
#include "tusb.h"
#include "ff.h"

#include "nes_lcd_gc9107.h"
#include "nes_buttons.h"
#include "nes_audio_pwm.h"
#include "nes_flash_disk.h"
#include "nes_picker.h"
#include "nes_font.h"
#include "nes_run.h"

#define THUMBYNES_VERSION "0.8"

/* Static framebuffer + filesystem state. */
static uint16_t       fb[128 * 128];
FATFS                 g_fs;   /* exported to nes_picker for XIP mmap */
static nes_rom_entry  roms[NES_PICKER_MAX_ROMS];

/* Quick-resume: /.last stores the base name of the most recently
 * launched ROM. On boot, if the file exists AND MENU isn't held,
 * we jump straight back into that cart. Hold MENU at boot to skip
 * the auto-resume and go to the picker instead. */
#define LAST_PATH "/.last"

static int last_save(const char *name) {
    FIL f;
    if (f_open(&f, LAST_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;
    UINT bw = 0;
    f_write(&f, name, (UINT)strlen(name), &bw);
    f_close(&f);
    return 0;
}

static int last_load(char *out, size_t outsz) {
    FIL f;
    if (f_open(&f, LAST_PATH, FA_READ) != FR_OK) return -1;
    UINT br = 0;
    f_read(&f, out, (UINT)(outsz - 1), &br);
    f_close(&f);
    out[br] = 0;
    return (br > 0) ? 0 : -1;
}

extern volatile int      g_msc_ejected;
extern volatile uint64_t g_msc_last_op_us;

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

/* Boot splash: shown for ~1.5 s after the hardware comes up. Pure
 * text — no logo bitmap to vendor. Doubles as the version readout. */
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
    sleep_ms(1500);
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

/* Pump USB + drain + scan loop, returning the index of a chosen ROM. */
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
         * itself, so we won't lose USB while it's up. */
        int sel = nes_picker_run(fb, roms, n_roms);
        if (sel >= 0) return sel;
        /* sel < 0 → MENU pressed; stay in lobby and rescan. */
        next_scan = make_timeout_time_ms(0);
    }
}

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
    splash_color(0x194a);   /* dim blue = LCD alive */

    /* FAT must come up BEFORE USB. If we let the host start
     * enumerating while mkfs is mid-flight, Windows reads
     * inconsistent BPB state and File Explorer trips over the
     * half-written directory tree. */
    splash_color(0xfd00);   /* orange = mounting / formatting */
    nes_flash_disk_init();
    if (boot_filesystem() < 0) {
        splash_color(0xf81f);   /* magenta = mount/format failed */
        while (1) tight_loop_contents();
    }
    /* Drain mkfs writes to flash before the host can read the disk. */
    nes_flash_disk_flush();

    /* USB stack. Disk is fully on flash → enumeration is safe. */
    splash_color(0x07ff);   /* cyan = USB stack starting */
    tusb_init();
    {
        absolute_time_t until = make_timeout_time_ms(1000);
        while (!time_reached(until)) {
            tud_task();
            sleep_us(100);
        }
    }
    splash_color(0x07E0);   /* green = ready */
    sleep_ms(120);

    boot_splash();

    /* Quick-resume: if /.last names a ROM that still exists, AND
     * MENU isn't being held at boot, hand straight off to the
     * runner. Holding MENU bypasses to the picker. */
    if (!nes_buttons_menu_pressed()) {
        char last[NES_PICKER_NAME_MAX];
        if (last_load(last, sizeof(last)) == 0) {
            char path[80];
            snprintf(path, sizeof(path), "/%s", last);
            FIL f;
            if (f_open(&f, path, FA_READ) == FR_OK) {
                f_close(&f);
                nes_run_rom(last, fb);
                /* Fall through to the lobby on exit. */
                while (nes_buttons_menu_pressed()) sleep_ms(10);
            }
        }
    }

    /* Lobby + picker. Loops forever in Phase 3. */
    while (1) {
        int sel = lobby();
        if (sel < 0) continue;

        /* Remember this ROM for quick-resume on next boot. */
        last_save(roms[sel].name);

        /* Hand off to the Nofrendo runner. Returns when the user
         * holds MENU; we then fall back through the lobby for
         * another pick. */
        int rc = nes_run_rom(roms[sel].name, fb);
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
