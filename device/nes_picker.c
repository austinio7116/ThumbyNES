/*
 * ThumbyNES — ROM picker (Phase 3 / 5).
 *
 * Phase 3 ships the file scanner and a "no ROMs" splash. The
 * interactive list view (Phase 5) is wired up here too — it draws
 * a paged list of file names and lets the user pick one with the
 * D-pad and A button. If the disk is empty the splash stays up
 * and the main loop keeps pumping USB so the user can drag ROMs
 * onto the volume; once tud_msc activity goes idle and a rescan
 * finds files, the picker swaps from splash → list automatically.
 */
#include "nes_picker.h"
#include "nes_font.h"
#include "nes_lcd_gc9107.h"
#include "nes_buttons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "tusb.h"
#include "ff.h"

#define FB_W 128
#define FB_H 128

extern volatile uint64_t g_msc_last_op_us;

/* --- file scan ------------------------------------------------------ */

int nes_picker_scan(nes_rom_entry *out, int max) {
    DIR dir;
    FILINFO info;
    if (f_opendir(&dir, "/") != FR_OK) return 0;
    int n = 0;
    while (n < max && f_readdir(&dir, &info) == FR_OK) {
        if (info.fname[0] == 0) break;
        if (info.fattrib & AM_DIR) continue;
        size_t L = strlen(info.fname);
        if (L < 4) continue;
        if (strcasecmp(info.fname + L - 4, ".nes") != 0) continue;
        strncpy(out[n].name, info.fname, NES_PICKER_NAME_MAX - 1);
        out[n].name[NES_PICKER_NAME_MAX - 1] = 0;
        out[n].size = (uint32_t)info.fsize;
        n++;
    }
    f_closedir(&dir);
    return n;
}

uint8_t *nes_picker_load_rom(const char *name, size_t *out_len) {
    char path[80];
    snprintf(path, sizeof(path), "/%s", name);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return NULL;
    FSIZE_t sz = f_size(&f);
    if (sz < 16 || sz > 1024 * 1024) { f_close(&f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { f_close(&f); return NULL; }
    UINT br;
    if (f_read(&f, buf, (UINT)sz, &br) != FR_OK || br != sz) {
        free(buf); f_close(&f); return NULL;
    }
    f_close(&f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

/* --- drawing helpers ----------------------------------------------- */

static void fb_clear(uint16_t *fb, uint16_t c) {
    for (int i = 0; i < FB_W * FB_H; i++) fb[i] = c;
}

static void fb_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if ((unsigned)yy >= FB_H) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if ((unsigned)xx >= FB_W) continue;
            fb[yy * FB_W + xx] = c;
        }
    }
}

#define COL_BG     0x0000   /* black */
#define COL_FG     0xFFFF   /* white */
#define COL_DIM    0x8410   /* mid grey */
#define COL_HIGHLT 0x07E0   /* green */
#define COL_TITLE  0xFD20   /* orange */
#define COL_ERR    0xF800   /* red */

/* --- splash screens ------------------------------------------------ */

static void draw_no_roms_splash(uint16_t *fb) {
    fb_clear(fb, COL_BG);
    nes_font_draw(fb, "ThumbyNES", 32, 30, COL_TITLE);
    nes_font_draw(fb, "no roms found",  20, 50, COL_FG);
    nes_font_draw(fb, "drag .nes files", 14, 70, COL_DIM);
    nes_font_draw(fb, "to usb drive",    22, 78, COL_DIM);
    nes_font_draw(fb, "then eject",      28, 92, COL_DIM);
}

/* --- list UI ------------------------------------------------------- */

static void draw_list(uint16_t *fb, const nes_rom_entry *e, int n,
                      int sel, int top) {
    fb_clear(fb, COL_BG);
    nes_font_draw(fb, "ThumbyNES", 32, 2, COL_TITLE);
    fb_rect(fb, 0, 10, 128, 1, COL_DIM);
    const int row_h = 8;
    const int max_rows = (FB_H - 14) / row_h;
    for (int i = 0; i < max_rows && (top + i) < n; i++) {
        int idx = top + i;
        int y = 14 + i * row_h;
        uint16_t fg = (idx == sel) ? COL_HIGHLT : COL_FG;
        if (idx == sel) fb_rect(fb, 0, y - 1, 128, 7, 0x18C3);
        char line[20];
        /* Truncate name to ~18 chars */
        snprintf(line, sizeof(line), "%.18s", e[idx].name);
        nes_font_draw(fb, line, 4, y, fg);
    }
    /* Footer */
    char ft[24];
    snprintf(ft, sizeof(ft), "%d/%d  A=play", sel + 1, n);
    nes_font_draw(fb, ft, 4, FB_H - 8, COL_DIM);
}

/* --- public entry --------------------------------------------------- */

int nes_picker_run(uint16_t *fb,
                    const nes_rom_entry *entries, int n_entries) {
    int sel = 0, top = 0;
    const int row_h = 8;
    const int max_rows = (FB_H - 14) / row_h;

    /* No ROMs: stay in splash, pump USB, exit when caller has files. */
    if (n_entries == 0) {
        draw_no_roms_splash(fb);
        nes_lcd_present(fb);
        nes_lcd_wait_idle();
        return -1;
    }

    /* Edge-detect input. */
    uint8_t prev = 0;
    while (1) {
        uint8_t b = nes_buttons_read();
        uint8_t pressed = b & ~prev;
        prev = b;

        /* nes_buttons_read returns the PICO-8 LRUDOX layout:
         *   bit0=L bit1=R bit2=U bit3=D bit4=O(A) bit5=X(B)
         */
        if (pressed & 0x04) { /* up */
            if (sel > 0) sel--;
            if (sel < top) top = sel;
        }
        if (pressed & 0x08) { /* down */
            if (sel < n_entries - 1) sel++;
            if (sel >= top + max_rows) top = sel - max_rows + 1;
        }
        if (pressed & 0x10) { /* O = A button */
            return sel;
        }
        if (nes_buttons_menu_pressed()) return -1;

        draw_list(fb, entries, n_entries, sel, top);
        nes_lcd_wait_idle();
        nes_lcd_present(fb);

        /* Keep USB alive while picker is up so user can still
         * drop more ROMs in without rebooting. */
        tud_task();

        sleep_ms(16);
    }
}
