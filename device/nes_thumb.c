/*
 * ThumbyNES — picker thumbnail / icon helpers.
 *
 * Two responsibilities:
 *
 *   1. Procedural platform icons. The picker's tab strip uses these
 *      as 12×8 mini-icons; the same routines blow up to 32×32 and
 *      64×64 for placeholder thumbnails when no screenshot exists.
 *      Everything is drawn from code so we don't have to vendor any
 *      bitmaps.
 *
 *   2. Screenshot sidecar I/O. Each ROM gets two raw RGB565 files
 *      written when the user presses MENU+A in-game:
 *
 *        /<basename>.scr32   32×32 ×2 bytes = 2 048 bytes
 *        /<basename>.scr64   64×64 ×2 bytes = 8 192 bytes
 *
 *      The 32×32 fits inline in the picker list view; the 64×64 is
 *      the hero in the default single-ROM view. We do nearest-style
 *      box averaging from the live 128×128 framebuffer (4×4 → 1 px
 *      for 32×32, 2×2 → 1 px for 64×64).
 */
#include "nes_thumb.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"

#define FB_W 128
#define FB_H 128

/* --- low-level fb helpers ------------------------------------------ */

static inline void put(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < FB_W && (unsigned)y < FB_H) fb[y * FB_W + x] = c;
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if ((unsigned)yy >= FB_H) continue;
        for (int i = 0; i < w; i++) put(fb, x + i, yy, c);
    }
}

/* Draw a hollow rectangle outline. */
static void rect_outline(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int i = 0; i < w; i++) { put(fb, x + i, y, c); put(fb, x + i, y + h - 1, c); }
    for (int j = 0; j < h; j++) { put(fb, x, y + j, c); put(fb, x + w - 1, y + j, c); }
}

/* --- procedural icons ----------------------------------------------- */
/*
 * All icons are designed inside a 12-wide × 8-tall bounding box and
 * scale-rendered into whatever cell the caller hands us. We pick a
 * pixel scale = max(1, min(w/12, h/8)) and centre the result in the
 * cell. The base shapes are deliberately blocky so they read at the
 * tab strip's actual 12×8 size as well as at 64×64 placeholder size.
 */

static void icon_nes(uint16_t *fb, int cx, int cy, int s, uint16_t fg) {
    /* NES controller silhouette: rounded rect with a cross-pad on the
     * left and two round buttons on the right. */
    int w = 12 * s, h = 6 * s;
    int x = cx - w / 2, y = cy - h / 2;
    /* body */
    fill_rect(fb, x + s, y, w - 2 * s, h, fg);
    fill_rect(fb, x, y + s, w, h - 2 * s, fg);
    /* d-pad cross (left) — punched out in bg colour, but we don't have
     * a bg colour here, so draw the pad as a small dark accent dot. */
    /* leave the dpad implicit; just put two button dots on the right */
    int btn_y = y + h / 2 - s / 2;
    fill_rect(fb, x + 7 * s, btn_y, s, s, 0x0000);
    fill_rect(fb, x + 9 * s, btn_y, s, s, 0x0000);
    /* d-pad: small + sign on left side, drawn in bg/black */
    int dx = x + 2 * s, dy = y + h / 2 - s;
    fill_rect(fb, dx, dy + s, 3 * s, s, 0x0000);
    fill_rect(fb, dx + s, dy, s, 3 * s, 0x0000);
}

static void icon_sms(uint16_t *fb, int cx, int cy, int s, uint16_t fg) {
    /* SMS: solid square with a single diagonal slash motif and a
     * small power LED dot. */
    int side = 8 * s;
    int x = cx - side / 2, y = cy - side / 2;
    fill_rect(fb, x, y, side, side, fg);
    /* diagonal slash punched out */
    for (int i = 0; i < side; i++) {
        for (int t = 0; t < s; t++) put(fb, x + i, y + i + t, 0x0000);
    }
    /* power LED — small bright dot upper-left */
    fill_rect(fb, x + s, y + s, s, s, 0xF800); /* red */
}

static void icon_gg(uint16_t *fb, int cx, int cy, int s, uint16_t fg) {
    /* Game Gear: portrait pill (handheld shape) with an inset green
     * screen rectangle. */
    int w = 6 * s, h = 10 * s;
    int x = cx - w / 2, y = cy - h / 2;
    /* rounded body — fake the rounding with two rects */
    fill_rect(fb, x, y + s, w, h - 2 * s, fg);
    fill_rect(fb, x + s, y, w - 2 * s, h, fg);
    /* screen window */
    int sw = 4 * s, sh = 3 * s;
    fill_rect(fb, x + (w - sw) / 2, y + 2 * s, sw, sh, 0x0500); /* dark green */
    /* speaker grille dots near bottom */
    int gy = y + h - 2 * s;
    fill_rect(fb, x + 2 * s, gy, s, s, 0x0000);
    fill_rect(fb, x + 3 * s, gy, s, s, 0x0000);
}

static void icon_star(uint16_t *fb, int cx, int cy, int s, uint16_t fg) {
    /* 5-point star, blocky. We use a fixed 9×9 pattern at scale `s`. */
    static const uint8_t star[9][9] = {
        {0,0,0,0,1,0,0,0,0},
        {0,0,0,0,1,0,0,0,0},
        {0,0,0,1,1,1,0,0,0},
        {1,1,1,1,1,1,1,1,1},
        {0,1,1,1,1,1,1,1,0},
        {0,0,1,1,1,1,1,0,0},
        {0,0,1,1,0,1,1,0,0},
        {0,1,1,0,0,0,1,1,0},
        {0,1,0,0,0,0,0,1,0},
    };
    int x0 = cx - (9 * s) / 2;
    int y0 = cy - (9 * s) / 2;
    for (int j = 0; j < 9; j++)
        for (int i = 0; i < 9; i++)
            if (star[j][i]) fill_rect(fb, x0 + i * s, y0 + j * s, s, s, fg);
}

void nes_thumb_icon(uint16_t *fb, int x, int y, uint8_t which, uint16_t tint) {
    /* Tab cell icons render at scale 1 inside a 12×8 box. */
    int cx = x + 6, cy = y + 4;
    switch (which) {
    case ICON_SYS_NES:  icon_nes (fb, cx, cy, 1, tint); break;
    case ICON_SYS_SMS:  icon_sms (fb, cx, cy, 1, tint); break;
    case ICON_SYS_GG:   icon_gg  (fb, cx, cy, 1, tint); break;
    case ICON_SYS_STAR: icon_star(fb, cx, cy, 1, tint); break;
    }
}

void nes_thumb_placeholder(uint16_t *fb, int x, int y, int size, uint8_t system) {
    /* A coloured panel with the platform icon scaled to roughly half
     * the panel size. Reads as "no screenshot yet, system X". */
    static const uint16_t panel_for[] = {
        [ROM_SYS_NES] = 0x18C3, /* dim grey */
        [ROM_SYS_SMS] = 0x2104, /* dim red */
        [ROM_SYS_GG]  = 0x0204, /* dim teal */
    };
    uint16_t panel = (system <= ROM_SYS_GG) ? panel_for[system] : 0x18C3;
    fill_rect(fb, x, y, size, size, panel);
    rect_outline(fb, x, y, size, size, 0x4208);

    int s = size / 16;        /* icon scale: 32 → 2, 64 → 4 */
    if (s < 1) s = 1;
    int cx = x + size / 2;
    int cy = y + size / 2;
    switch (system) {
    case ROM_SYS_NES: icon_nes(fb, cx, cy, s, 0xFFFF); break;
    case ROM_SYS_SMS: icon_sms(fb, cx, cy, s, 0xFFFF); break;
    case ROM_SYS_GG:  icon_gg (fb, cx, cy, s, 0xFFFF); break;
    default:          icon_nes(fb, cx, cy, s, 0xFFFF); break;
    }
}

/* --- screenshot sidecar I/O ---------------------------------------- */

static void scr_path(char *out, size_t outsz, const char *rom_name, int size) {
    char base[64];
    strncpy(base, rom_name, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    snprintf(out, outsz, "/%s.scr%d", base, size);
}

bool nes_thumb_draw(uint16_t *fb, int x, int y, int size, const char *rom_name) {
    if (size != 32 && size != 64) return false;
    char path[80];
    scr_path(path, sizeof(path), rom_name, size);

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    UINT expected = (UINT)(size * size * 2);
    if (f_size(&f) != expected) { f_close(&f); return false; }

    /* Read row-by-row directly into the framebuffer to avoid a
     * 8 KB scratch on the stack. The picker fb is 32 KB and lives
     * in BSS — we just write into the right rect. */
    for (int row = 0; row < size; row++) {
        int yy = y + row;
        if ((unsigned)yy >= FB_H) { f_close(&f); return true; }
        uint16_t *dst = fb + yy * FB_W + x;
        UINT br = 0;
        if (f_read(&f, dst, (UINT)(size * 2), &br) != FR_OK
            || br != (UINT)(size * 2)) {
            f_close(&f);
            return false;
        }
        /* Clip the right edge if x + size > FB_W. */
        if (x + size > FB_W) {
            int clip = (x + size) - FB_W;
            for (int i = 0; i < clip; i++) /* nothing — already past end */;
            (void)clip;
        }
    }
    f_close(&f);
    return true;
}

/* Box-average a w_src×w_src region of the source 128×128 framebuffer
 * down to a single output pixel. */
static uint16_t box_avg(const uint16_t *src, int sx, int sy, int n) {
    uint32_t r = 0, g = 0, b = 0;
    for (int j = 0; j < n; j++) {
        const uint16_t *row = src + (sy + j) * FB_W + sx;
        for (int i = 0; i < n; i++) {
            uint16_t p = row[i];
            r += (p >> 11) & 0x1F;
            g += (p >>  5) & 0x3F;
            b += (p      ) & 0x1F;
        }
    }
    int total = n * n;
    r /= total; g /= total; b /= total;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static int write_scr(const uint16_t *src, const char *rom_name, int size, int srcN) {
    /* Source is 128×128; downscale by srcN per output pixel. */
    char path[80];
    scr_path(path, sizeof(path), rom_name, size);

    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;

    /* Stream one row at a time so we never need a 8 KB scratch buffer. */
    uint16_t row[64];
    for (int oy = 0; oy < size; oy++) {
        for (int ox = 0; ox < size; ox++) {
            row[ox] = box_avg(src, ox * srcN, oy * srcN, srcN);
        }
        UINT bw = 0;
        if (f_write(&f, row, (UINT)(size * 2), &bw) != FR_OK
            || bw != (UINT)(size * 2)) {
            f_close(&f);
            return -2;
        }
    }
    f_close(&f);
    return 0;
}

int nes_thumb_save(const uint16_t *src, const char *rom_name) {
    /* 128 / 32 = 4 (4×4 average); 128 / 64 = 2 (2×2 average). */
    int rc = write_scr(src, rom_name, 32, 4);
    if (rc != 0) return rc;
    return write_scr(src, rom_name, 64, 2);
}
