/*
 * ThumbyNES — generic in-game pause menu.
 *
 * Implementation notes
 * --------------------
 *  - The menu darkens the existing fb in place by halving every
 *    pixel's RGB565 channels (right-shift then mask to drop the LSB
 *    of each channel). The result is a uniformly dimmer copy of the
 *    last game frame. Cheap (one pass over 32 KB) and looks much
 *    better than a flat-fill backdrop.
 *
 *  - The menu panel is a translucent dark rect with a thin orange
 *    title bar and a footer hint. Items render as `label .. value`
 *    rows with the cursor row highlighted in green.
 *
 *  - More than 9 items scroll: the cursor moves through the full
 *    list, the visible window slides to keep the cursor on screen,
 *    and small ▲ ▼ glyphs appear in the title bar when there's
 *    content above/below.
 */
#include "nes_menu.h"
#include "nes_font.h"
#include "nes_lcd_gc9107.h"
#include "nes_buttons.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"

#define FB_W 128
#define FB_H 128

#define BTN_LEFT_GP   0
#define BTN_UP_GP     1
#define BTN_RIGHT_GP  2
#define BTN_DOWN_GP   3
#define BTN_LB_GP     6
#define BTN_A_GP     21
#define BTN_RB_GP    22
#define BTN_B_GP     25
#define BTN_MENU_GP  26

/* Layout constants. */
#define TITLE_H      11
#define SUBTITLE_H   8
#define FOOTER_H     8
#define ROW_H        10
#define ITEMS_TOP    (TITLE_H + SUBTITLE_H + 1)
#define ITEMS_BOTTOM (FB_H - FOOTER_H - 1)
#define VISIBLE_ROWS ((ITEMS_BOTTOM - ITEMS_TOP) / ROW_H)

/* Colours (RGB565). */
#define COL_BG       0x0000
#define COL_FG       0xFFFF
#define COL_DIM      0x8410
#define COL_HIGHLT   0x07E0    /* green cursor row */
#define COL_TITLE    0xFD20    /* orange */
#define COL_DARK     0x4208    /* very dim grey */
#define COL_DISABLED 0x4208

/* --- helpers -------------------------------------------------------- */

static inline void put_pixel(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < FB_W && (unsigned)y < FB_H) fb[y * FB_W + x] = c;
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
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

/* In-place darken the framebuffer to ~1/4 brightness. Channel-correct
 * — extract each channel, shift right by 2, repack. Eight ops per
 * pixel × 16 K pixels at 250 MHz is irrelevant. */
static void darken_fb(uint16_t *fb) {
    for (int i = 0; i < FB_W * FB_H; i++) {
        uint16_t p = fb[i];
        uint32_t r = (p >> 11) & 0x1F;
        uint32_t g = (p >>  5) & 0x3F;
        uint32_t b = (p      ) & 0x1F;
        r >>= 2; g >>= 2; b >>= 2;
        fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

/* Draw a horizontal slider bar inside (x, y, w, h) representing
 * `value` between `vmin` and `vmax`. */
static void draw_slider(uint16_t *fb, int x, int y, int w, int h,
                         int value, int vmin, int vmax, uint16_t fg) {
    fill_rect(fb, x, y, w, h, COL_DARK);
    /* 1px outline */
    for (int i = 0; i < w; i++) { put_pixel(fb, x + i, y, fg); put_pixel(fb, x + i, y + h - 1, fg); }
    for (int j = 0; j < h; j++) { put_pixel(fb, x, y + j, fg); put_pixel(fb, x + w - 1, y + j, fg); }
    int span = vmax - vmin;
    if (span <= 0) return;
    int v = value - vmin;
    if (v < 0) v = 0;
    if (v > span) v = span;
    int fill_w = ((w - 2) * v) / span;
    fill_rect(fb, x + 1, y + 1, fill_w, h - 2, fg);
}

/* Find the next selectable item (skipping disabled rows) starting
 * from `from` in direction `dir` (+1 or −1). Returns the new index,
 * or `from` if nothing is found. */
static int seek_selectable(const nes_menu_item_t *items, int n, int from, int dir) {
    int i = from;
    for (int tries = 0; tries < n; tries++) {
        i += dir;
        if (i < 0)  i = n - 1;
        if (i >= n) i = 0;
        if (items[i].enabled) return i;
    }
    return from;
}

/* --- main draw ----------------------------------------------------- */

static void draw_menu(uint16_t       *fb_dim,    /* the darkened backdrop */
                       uint16_t       *fb,        /* live framebuffer to draw into */
                       const char     *title,
                       const char     *subtitle,
                       const nes_menu_item_t *items,
                       int             n_items,
                       int             cursor,
                       int             scroll_top) {
    /* Restore the dimmed backdrop. */
    memcpy(fb, fb_dim, FB_W * FB_H * 2);

    /* Title bar */
    fill_rect(fb, 0, 0, FB_W, TITLE_H, COL_BG);
    fill_rect(fb, 0, TITLE_H - 1, FB_W, 1, COL_TITLE);
    if (title) {
        nes_font_draw(fb, title, 2, 2, COL_TITLE);
    }

    /* Scroll arrows in the title bar */
    if (scroll_top > 0) {
        nes_font_draw(fb, "^", FB_W - 14, 2, COL_TITLE);
    }
    if (scroll_top + VISIBLE_ROWS < n_items) {
        nes_font_draw(fb, "v", FB_W - 7, 2, COL_TITLE);
    }

    /* Subtitle (cart name) */
    if (subtitle) {
        char buf[24];
        strncpy(buf, subtitle, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        if (strlen(buf) > 21) buf[21] = 0;
        nes_font_draw(fb, buf, 2, TITLE_H, COL_DIM);
    }

    /* Items */
    for (int row = 0; row < VISIBLE_ROWS; row++) {
        int idx = scroll_top + row;
        if (idx >= n_items) break;
        const nes_menu_item_t *it = &items[idx];

        int y = ITEMS_TOP + row * ROW_H;
        bool is_cursor = (idx == cursor);

        if (is_cursor) {
            fill_rect(fb, 0, y - 1, FB_W, ROW_H, 0x0220 /* dim green */);
        }

        uint16_t fg = is_cursor          ? COL_HIGHLT
                    : !it->enabled       ? COL_DISABLED
                                          : COL_FG;

        /* Cursor arrow on cursor row */
        if (is_cursor) nes_font_draw(fb, ">", 1, y + 1, fg);

        /* Label */
        nes_font_draw(fb, it->label, 7, y + 1, fg);

        /* Value column at right edge */
        char val[24] = {0};
        switch (it->kind) {
        case NES_MENU_KIND_ACTION:
            /* No value column for buttons. */
            break;
        case NES_MENU_KIND_TOGGLE:
            snprintf(val, sizeof(val), (*it->value_ptr) ? "ON" : "OFF");
            break;
        case NES_MENU_KIND_SLIDER: {
            /* Draw bar instead of text. Bar is 28 px wide. */
            int bar_x = FB_W - 32;
            int bar_y = y + 1;
            draw_slider(fb, bar_x, bar_y, 28, ROW_H - 2,
                         *it->value_ptr, it->min, it->max, fg);
            /* And a tiny numeric tail to the right of the bar... no
             * room. Skip. */
            break;
        }
        case NES_MENU_KIND_CHOICE:
            if (it->choices && *it->value_ptr >= 0
                && *it->value_ptr < it->num_choices) {
                snprintf(val, sizeof(val), "%s", it->choices[*it->value_ptr]);
            }
            break;
        }

        if (val[0] || it->suffix) {
            char combined[40];
            if (it->suffix) {
                snprintf(combined, sizeof(combined), "%s %s", val, it->suffix);
            } else {
                snprintf(combined, sizeof(combined), "%s", val);
            }
            int vw = nes_font_width(combined);
            nes_font_draw(fb, combined, FB_W - vw - 2, y + 1, fg);
        }
    }

    /* Footer */
    fill_rect(fb, 0, FB_H - FOOTER_H, FB_W, FOOTER_H, COL_BG);
    fill_rect(fb, 0, FB_H - FOOTER_H, FB_W, 1, COL_TITLE);
    const nes_menu_item_t *cur = (cursor >= 0 && cursor < n_items) ? &items[cursor] : NULL;
    const char *hint =
        (!cur || cur->kind == NES_MENU_KIND_ACTION) ? "A select  B back" :
        (cur->kind == NES_MENU_KIND_TOGGLE)         ? "<> toggle  B back" :
        (cur->kind == NES_MENU_KIND_SLIDER)         ? "<> adjust  B back" :
                                                       "<> change  B back";
    int hw = nes_font_width(hint);
    nes_font_draw(fb, hint, (FB_W - hw) / 2, FB_H - FOOTER_H + 1, COL_DIM);
}

/* --- main loop ----------------------------------------------------- */

nes_menu_result_t nes_menu_run(uint16_t       *fb,
                                const char     *title,
                                const char     *subtitle,
                                nes_menu_item_t *items,
                                int             n_items) {
    nes_menu_result_t result = { .kind = NES_MENU_RESUME, .action_id = 0 };

    if (n_items == 0) return result;

    /* Snapshot + dim the current frame as a backdrop. */
    static uint16_t fb_dim[FB_W * FB_H];
    memcpy(fb_dim, fb, sizeof(fb_dim));
    darken_fb(fb_dim);

    /* Initial cursor — first selectable item. */
    int cursor = -1;
    for (int i = 0; i < n_items; i++) {
        if (items[i].enabled) { cursor = i; break; }
    }
    if (cursor < 0) return result;
    int scroll_top = 0;

    /* Wait for the MENU long-press release that triggered the menu
     * so the very first frame doesn't immediately react to the same
     * MENU press. */
    while (!gpio_get(BTN_MENU_GP)) {
        tud_task();
        sleep_ms(10);
    }

    int prev_lt = 0, prev_rt = 0, prev_up = 0, prev_dn = 0;
    int prev_a = 0, prev_b = 0, prev_menu = 0;

    while (1) {
        int lt = !gpio_get(BTN_LEFT_GP);
        int rt = !gpio_get(BTN_RIGHT_GP);
        int up = !gpio_get(BTN_UP_GP);
        int dn = !gpio_get(BTN_DOWN_GP);
        int a  = !gpio_get(BTN_A_GP);
        int b  = !gpio_get(BTN_B_GP);
        int mn = !gpio_get(BTN_MENU_GP);

        int e_lt = lt && !prev_lt;
        int e_rt = rt && !prev_rt;
        int e_up = up && !prev_up;
        int e_dn = dn && !prev_dn;
        int e_a  = a  && !prev_a;
        int e_b  = b  && !prev_b;
        int e_mn = mn && !prev_menu;

        prev_lt = lt; prev_rt = rt; prev_up = up; prev_dn = dn;
        prev_a  = a;  prev_b  = b;  prev_menu = mn;

        /* B or MENU = close menu without action. Drain the same
         * way to avoid the runner re-reading the press. */
        if (e_b || e_mn) {
            while (!gpio_get(BTN_B_GP) || !gpio_get(BTN_MENU_GP)) {
                tud_task(); sleep_ms(10);
            }
            return result;
        }

        /* UP / DOWN navigate. */
        if (e_up) cursor = seek_selectable(items, n_items, cursor, -1);
        if (e_dn) cursor = seek_selectable(items, n_items, cursor, +1);

        /* Keep cursor on screen. */
        if (cursor < scroll_top) scroll_top = cursor;
        if (cursor >= scroll_top + VISIBLE_ROWS) {
            scroll_top = cursor - VISIBLE_ROWS + 1;
        }
        if (scroll_top < 0) scroll_top = 0;
        if (scroll_top > n_items - VISIBLE_ROWS && n_items >= VISIBLE_ROWS) {
            scroll_top = n_items - VISIBLE_ROWS;
        }
        if (n_items < VISIBLE_ROWS) scroll_top = 0;

        nes_menu_item_t *it = &items[cursor];

        /* LEFT / RIGHT adjust value (or A toggles bool). */
        if (it->enabled) switch (it->kind) {
        case NES_MENU_KIND_TOGGLE:
            if (e_lt || e_rt || e_a) {
                *it->value_ptr = !*it->value_ptr;
            }
            break;
        case NES_MENU_KIND_SLIDER:
            if (e_lt && *it->value_ptr > it->min) (*it->value_ptr)--;
            if (e_rt && *it->value_ptr < it->max) (*it->value_ptr)++;
            break;
        case NES_MENU_KIND_CHOICE:
            if (e_lt) {
                if (*it->value_ptr > 0) (*it->value_ptr)--;
                else                    *it->value_ptr = it->num_choices - 1;
            }
            if (e_rt) {
                if (*it->value_ptr < it->num_choices - 1) (*it->value_ptr)++;
                else                                       *it->value_ptr = 0;
            }
            break;
        case NES_MENU_KIND_ACTION:
            if (e_a) {
                result.kind = NES_MENU_ACTION;
                result.action_id = it->action_id;
                /* Drain the A press before returning so the
                 * caller (and any code the caller hands off to,
                 * e.g. the picker after Quit) doesn't immediately
                 * react to the same press. */
                while (!gpio_get(BTN_A_GP)) { tud_task(); sleep_ms(10); }
                return result;
            }
            break;
        }

        /* Repaint. */
        draw_menu(fb_dim, fb, title, subtitle, items, n_items, cursor, scroll_top);
        nes_lcd_wait_idle();
        nes_lcd_present(fb);

        tud_task();
        sleep_ms(16);
    }
}
